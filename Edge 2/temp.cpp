#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <functional>
#include <condition_variable>
#include <sstream>
#include <fstream>
#include <csignal>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <unordered_map>

using namespace std;

//////////////////////////////////////////
// 配置系统
struct Config{
    int thread_num = 4;
    int server_port = 8080;
    string log_level = "INFO";

    bool load(const string &filename){
        ifstream fin(filename);
        if(!fin.is_open()){
            return false;
        }

        string line;
        while(getline(fin,line)){
            if(line.empty() || line[0]=='#') continue;

            auto pos=line.find('=');
            if(pos==string::npos) continue;

            string key=line.substr(0,pos);
            string val=line.substr(pos+1);

            if(key=="thread_num") thread_num = stoi(val);
            else if(key=="server_port") server_port = stoi(val);
            else if(key=="log_level") log_level = val;
        }
        return true;
    }
};

//////////////////////////////////////////
// 日志系统
enum class LogLevel { DEBUG=0, INFO=1, WARN=2, ERROR=3 };
mutex ioMutex;
LogLevel g_log_level = LogLevel::INFO;

LogLevel str_to_level(const string &s){
    if(s=="DEBUG") return LogLevel::DEBUG;
    if(s=="INFO") return LogLevel::INFO;
    if(s=="WARN") return LogLevel::WARN;
    if(s=="ERROR") return LogLevel::ERROR;
    return LogLevel::INFO;
}

void log_line(LogLevel lvl, const string& msg){
    if(lvl < g_log_level) return;
    lock_guard<mutex> lock(ioMutex);
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    tm tm_buf{};
    localtime_r(&t,&tm_buf);
    char buf[32];
    strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&tm_buf);
    string lvl_str;
    switch(lvl){
        case LogLevel::DEBUG: lvl_str="DEBUG"; break;
        case LogLevel::INFO: lvl_str="INFO"; break;
        case LogLevel::WARN: lvl_str="WARN"; break;
        case LogLevel::ERROR: lvl_str="ERROR"; break;
    }
    cout << "[" << buf << "][" << lvl_str << "] " << msg << endl;
}

//////////////////////////////////////////
// 线程池
class ThreadPool{
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running{true};
public:
    ThreadPool(size_t threadNum){
        for(size_t i=0;i<threadNum;i++){
            workers.emplace_back([this](){
                while(true){
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        condvar.wait(lock,[this](){ return !tasks.empty()||!running; });
                        if(!running && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop();
                    }
                    try{ task(); }
                    catch(const exception& e){ log_line(LogLevel::ERROR,"Worker exception: "+string(e.what())); }
                    catch(...){ log_line(LogLevel::ERROR,"Worker unknown exception"); }
                }
            });
        }
    }

    ~ThreadPool(){ shutdown(); }

    void submit(function<void()> task){
        if(!running) throw runtime_error("ThreadPool stopped");
        {
            lock_guard<mutex> lock(queueMutex);
            tasks.push(move(task));
        }
        condvar.notify_one();
    }

    void shutdown(){
        running=false;
        condvar.notify_all();
        for(auto& t: workers) if(t.joinable()) t.join();
    }
};

//////////////////////////////////////////
// Service
class Service{
public:
    enum class State { INIT,RUNNING,STOPPING,STOPPED };
private:
    mutex stateMutex;
    State state;
    ThreadPool pool;
    thread mainThread;
    atomic<bool> running_flag{false};
public:
    Service(size_t threads):state(State::INIT), pool(threads){}

    ~Service(){ stop(); }

    void start(){
        lock_guard<mutex> lock(stateMutex);
        if(state != State::INIT) return;
        state=State::RUNNING;
        running_flag = true;
        log_line(LogLevel::INFO,"Service starting...");

        mainThread = thread([this](){
            int counter=0;
            while(running_flag){
                pool.submit([counter](){
                    log_line(LogLevel::DEBUG,"Executing Service Task "+to_string(counter));
                    this_thread::sleep_for(chrono::milliseconds(200));
                });
                counter++;
                this_thread::sleep_for(chrono::milliseconds(500));
            }
            log_line(LogLevel::INFO,"Service main thread exiting...");
            lock_guard<mutex> lock(stateMutex);
            state = State::STOPPED;
        });
    }

    void stop(){
        {
            lock_guard<mutex> lock(stateMutex);
            if(state==State::STOPPED) return;
            log_line(LogLevel::INFO,"Service stopping...");
            running_flag = false;
            state = State::STOPPING;
        }
        if(mainThread.joinable()) mainThread.join();
        pool.shutdown();
        lock_guard<mutex> lock(stateMutex);
        state = State::STOPPED;
        log_line(LogLevel::INFO,"Service stopped.");
    }

    State get_state(){
        lock_guard<mutex> lock(stateMutex);
        return state;
    }

    static string state_to_string(State s){
        switch(s){
            case State::INIT: return "INIT";
            case State::RUNNING: return "RUNNING";
            case State::STOPPING: return "STOPPING";
            case State::STOPPED: return "STOPPED";
        }
        return "UNKNOWN";
    }
};

Service *g_service=nullptr;
atomic<bool> g_running{true};

void signal_handler(int signal){
    log_line(LogLevel::WARN,"Signal received: "+to_string(signal));
    g_running = false;
    if(g_service) g_service->stop();
}

//////////////////////////////////////////
// EpollServer
int set_nonblocking(int fd){
    int flags = fcntl(fd,F_GETFL,0);
    if(flags==-1) return -1;
    return fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}

class EpollServer{
private:
    int listen_fd;
    int epfd;
    ThreadPool &pool;
    atomic<bool> running{true};
    unordered_map<int,string> buffers;
public:
    EpollServer(int port, ThreadPool &tp):pool(tp){
        listen_fd = socket(AF_INET,SOCK_STREAM,0);
        int opt=1;
        setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        bind(listen_fd,(sockaddr*)&addr,sizeof(addr));
        listen(listen_fd,128);

        set_nonblocking(listen_fd);
        epfd = epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev);

        log_line(LogLevel::INFO,"[Server] listening on port "+to_string(port));
    }

    void run(){
        vector<epoll_event> events(1024);
        while(running){
            int n = epoll_wait(epfd, events.data(), events.size(), 500); // 500ms 超时检查退出
            if(n<0) continue;
            for(int i=0;i<n;i++){
                int fd = events[i].data.fd;
                if(fd==listen_fd) accept_client();
                else handle_client(fd);
            }
        }
        close(listen_fd);
        log_line(LogLevel::INFO,"[Server] stopped.");
    }

    void stop(){ running=false; }

private:
    void accept_client(){
        while(true){
            sockaddr_in client{};
            socklen_t len=sizeof(client);
            int cfd = accept(listen_fd,(sockaddr*)&client,&len);
            if(cfd<0){
                if(errno==EAGAIN) break;
                perror("accept fail");
                continue;
            }
            set_nonblocking(cfd);
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = cfd;
            epoll_ctl(epfd,EPOLL_CTL_ADD,cfd,&ev);
            log_line(LogLevel::INFO,"[+] new client fd="+to_string(cfd));
        }
    }

    void close_client(int fd){
        epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);
        close(fd);
        buffers.erase(fd);
        log_line(LogLevel::INFO,"[-] client closed fd="+to_string(fd));
    }

    void handle_client(int fd){
        char buffer[1024];
        while(true){
            ssize_t n = read(fd,buffer,sizeof(buffer));
            if(n>0){
                buffers[fd].append(buffer,n);
                process_buffer(fd);
            }
            else if(n==0){ close_client(fd); break; }
            else {
                if(errno==EAGAIN || errno==EWOULDBLOCK) break;
                else { close_client(fd); break; }
            }
        }
    }

    void process_buffer(int fd){
        auto &buffer = buffers[fd];
        size_t pos;
        while((pos=buffer.find('\n'))!=string::npos){
            string msg = buffer.substr(0,pos);
            buffer.erase(0,pos+1);
            log_line(LogLevel::DEBUG,"[Reactor] fd="+to_string(fd)+" received msg:"+msg);

            pool.submit([fd,msg](){
                stringstream ss; ss << this_thread::get_id();
                log_line(LogLevel::DEBUG,"[Worker] thread="+ss.str()+" processing fd="+to_string(fd)+" msg="+msg);
            });
            string echo = msg + "\n";
            write(fd,echo.c_str(),echo.size());
        }
    }
};

//////////////////////////////////////////
// Main
int main(){
    Config cfg;
    if(!cfg.load("d17_config.ini")){
        log_line(LogLevel::WARN,"Failed to load d17_config.ini, using defaults.");
    }

    g_log_level = str_to_level(cfg.log_level);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    Service service(cfg.thread_num);
    g_service = &service;
    service.start();

    ThreadPool pool(cfg.thread_num);
    EpollServer server(cfg.server_port, pool);

    thread server_thread([&](){ server.run(); });

    // 主线程等待退出信号
    while(g_running) this_thread::sleep_for(chrono::milliseconds(100));

    // 停止服务
    server.stop();
    if(server_thread.joinable()) server_thread.join();

    log_line(LogLevel::INFO,"Program exiting gracefully.");
    return 0;
}