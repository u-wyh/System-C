#include<iostream>
#include<vector>
#include<thread>
#include<mutex>
#include<atomic>
#include<chrono>
#include<queue>
#include<functional>
#include<condition_variable>
#include<sstream>
#include<fstream>
#include<csignal>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<unistd.h>
#include<fcntl.h>
#include<cstring>
#include<unordered_map>

using namespace std;

struct Config{
    int thread_num=4;
    int server_port=8080;
    string log_file="server.log";
    int monitor_interval=1000;
    int log_batch=256;
    int log_flush=50;
};

enum class LogLevel{
    DEBUG=0,INFO=1,WARN=2,ERROR=3
};

class AsyncLogger{

private:
    vector<string>buffer1,buffer2;
    atomic<bool>swap_flag{false};
    mutex mtx;
    condition_variable cv;
    atomic<bool> running{true};
    thread worker;
    ofstream fout;
    LogLevel level;
    size_t batch;
    int flush;

public:
    string now_time(){
        auto now=chrono::system_clock::now();
        time_t t=chrono::system_clock::to_time_t(now);
        tm tm_buf{};
        localtime_r(&t,&tm_buf); // Linux 下线程安全
        char buf[32];
        strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&tm_buf);
        return string(buf);
    }

    void process(){
        vector<string>* current;
        vector<string>* back;

        while(running||!buffer1.empty()||!buffer2.empty()){
            {
                unique_lock<mutex> lock(mtx);
                cv.wait_for(lock,chrono::milliseconds(flush),[this](){
                    return !buffer1.empty()||!buffer2.empty()||!running;
                });
                swap_flag=!swap_flag;
                if(swap_flag){
                    current=&buffer1;
                    back=&buffer2;
                }
                else{
                    current=&buffer2;
                    back=&buffer1;
                }
                current->swap(*back);
            }
            for(auto &line:*current){
                cout<<line<<endl;
                if(fout.is_open()){
                    fout<<line<<endl;
                }
            }
            current->clear();
        }

    }

    AsyncLogger(const string &filename,LogLevel lvl=LogLevel::INFO,size_t batch_size=256,int flush_ms=50)
    :batch(batch_size),flush(flush_ms),level(lvl){
        fout.open(filename,ios::app);
        buffer1.reserve(batch*2);
        buffer2.reserve(batch*2);
        worker=thread(&AsyncLogger::process,this);
    }

    ~AsyncLogger(){
        shutdown();
    }

    void shutdown(){
        running=false;
        cv.notify_all();
        if(worker.joinable()){
            worker.join();
        }
        if(fout.is_open()){
            fout.close();
        }
    }

    void log(LogLevel lvl,const string& msg){
        if(lvl<level){
            return ;
        }
        string lvl_str;
        switch(lvl){
            case LogLevel::DEBUG: lvl_str="DEBUG";break;
            case LogLevel::INFO: lvl_str="INFO";break;
            case LogLevel::WARN: lvl_str="WARN";break;
            case LogLevel::ERROR: lvl_str="ERROR";break;
        }
        string line="["+now_time()+"]"+"["+lvl_str+"]"+msg;
        {
            lock_guard<mutex> lock(mtx);
            if(swap_flag){
                buffer1.push_back(move(line));
            }
            else{
                buffer2.push_back(move(line));
            }
        }
        cv.notify_one();
    }
};

AsyncLogger g_logger("server.log",LogLevel::INFO);

class ThreadPool{

private:
    // 线程池
    vector<thread>workers;
    // 任务列表，先进先出
    queue<function<void()>>tasks;

    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running;

public:
    ThreadPool(size_t threadNum):running(true){
        for(size_t i=0;i<threadNum;i++){
            workers.emplace_back([this,i](){
                while(true){
                    // task是个临时变量，结束后自动析构，workers线程可以接受新的任务
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        // 要么被显式解锁(notify_one/all)  要么被系统解锁，就是这个函数
                        condvar.wait(lock,[this](){
                            return !tasks.empty()||!running;
                        });
                        if(!running&&tasks.empty()){
                            return ;
                        }
                        task=move(tasks.front());
                        tasks.pop();
                    }

                    try{
                        task();
                    }
                    catch(const exception& e){
                        g_logger.log(LogLevel::ERROR,"[Worker] exception: "+string(e.what()));
                    }
                    catch(...){
                        g_logger.log(LogLevel::ERROR,"[Worker] unknown exception");
                    }
                }
            });
        }
    }

    ~ThreadPool(){
        shutdown();
    }

    void submit(function<void()>task){
        if (!running) {
            throw runtime_error("ThreadPool is stopped, cannot submit task.");
        }
        {
            lock_guard<mutex>lock(queueMutex);
            tasks.push(move(task));
        }
        condvar.notify_one();
    }

    void shutdown(){
        running=false;
        condvar.notify_all();
        for(auto& t:workers){
            if(t.joinable()){
                t.join();
            }
        }
    }
};

// 这是更安全的设置非阻塞式的方式
int set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    if(flags==-1){
        return -1;
    }
    return fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}

class EpollServer{

private:
    int listen_fd;
    int epfd;
    ThreadPool &pool;
    atomic<bool> running{true};
    unordered_map<int,string>buffers;
    atomic<int>tps{0};

public:
    EpollServer(int port,ThreadPool &tp):pool(tp){
        listen_fd=socket(AF_INET,SOCK_STREAM,0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
        listen(listen_fd, 512);

        set_nonblocking(listen_fd);

        epfd = epoll_create1(0);

        epoll_event ev{};
        ev.events = EPOLLIN|EPOLLET;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        g_logger.log(LogLevel::INFO,"[Server] listening on port "+to_string(port));
    }

    void run(){
        vector<epoll_event>events(1024);
        while(running){
            int n=epoll_wait(epfd,events.data(),events.size(),500);
            for(int i=0;i<n;i++){
                int fd=events[i].data.fd;
                if(fd==listen_fd){
                    accept_client();
                }
                else{
                    handle_client(fd);
                }
            }
        }
        close(listen_fd);
        g_logger.log(LogLevel::INFO,"[Server] stopped.");
    }

    void stop(){
        running=false;
    }

    void accept_client(){
        while (true){
            sockaddr_in client{};
            socklen_t len=sizeof(client);
            int cfd=accept(listen_fd,(sockaddr*)&client,&len);
            if(cfd<0){
                if(errno==EAGAIN){
                    break;
                }
                perror("accept fail");
                continue;
            }
            set_nonblocking(cfd);

            epoll_event ev{};
            ev.events=EPOLLIN|EPOLLET;
            ev.data.fd=cfd;
            epoll_ctl(epfd,EPOLL_CTL_ADD,cfd,&ev);

            // g_logger.log(LogLevel::INFO,"[+] new client fd="+to_string(cfd));
        }
    }

    void close_client(int fd){
        epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);
        close(fd);
        buffers.erase(fd);
        // g_logger.log(LogLevel::INFO,"[-] client close fd="+to_string(fd));
    }

    void handle_client(int fd){
        char buffer[1024];

        while(true){
            ssize_t n=read(fd,buffer,sizeof(buffer));

            if(n>0){
                buffers[fd].append(buffer,n);
                process_buffer(fd);
            } 
            else if(n==0){
                close_client(fd);
                break;
            }
            else{
                if(errno==EAGAIN||errno==EWOULDBLOCK){
                    break;
                }
                else{
                    perror("read error");
                    close_client(fd);
                    break;
                }
            }
        }
    }

    void process_buffer(int fd){
        auto& buffer=buffers[fd];
        size_t pos;
        while((pos=buffer.find('\n'))!=string::npos){
            string msg=buffer.substr(0,pos);
            buffer.erase(0,pos+1);
            tps++;
            // g_logger.log(LogLevel::DEBUG,"[Reactor] fd="+to_string(fd)+" received msg:"+msg);

            pool.submit([fd,msg](){
                stringstream ss;
                ss<<this_thread::get_id();
                g_logger.log(LogLevel::DEBUG,"[Worker] thread="+ss.str()+" start proccesing fd = "+to_string(fd)+" msg="+msg);
            });
            string echo=msg+"\n";
            write(fd,echo.c_str(),echo.size());
        }
    }

    int get_tps(){
        int t=tps.load();
        tps=0;
        return t;
    }
};

void monitor(EpollServer &server,atomic<bool>&running,int interval){
    uint64_t lastTotalTime = 0;
    auto lastCheck = chrono::steady_clock::now();

    while (running) {
        this_thread::sleep_for(chrono::milliseconds(interval));

        // CPU
        ifstream fstat("/proc/self/stat");
        string tmp;
        uint64_t utime = 0, stime = 0;
        for (int i = 0; i < 13; i++) fstat >> tmp;
        fstat >> utime >> stime;
        uint64_t totalTime = utime + stime;

        auto now = chrono::steady_clock::now();
        double elapsedSec = chrono::duration<double>(now - lastCheck).count();
        double cpuUsage = 0.0;
        if (lastTotalTime != 0 && elapsedSec > 0)
            cpuUsage = (double)(totalTime - lastTotalTime) / sysconf(_SC_CLK_TCK) / elapsedSec * 100.0;
        lastTotalTime = totalTime;
        lastCheck = now;

        // Mem
        ifstream fstatus("/proc/self/status");
        string line; size_t memKB = 0;
        while (getline(fstatus, line)) {
            if (line.find("VmRSS:") != string::npos) {
                stringstream ss(line);
                string key; ss >> key >> memKB;
            }
        }

        // TPS
        uint64_t tps = server.get_tps();

        g_logger.log(LogLevel::INFO, "[Monitor] CPU=" + to_string(cpuUsage) +
                                      "% Mem=" + to_string(memKB / 1024.0) + "MB TPS=" + to_string(tps));
    }
}

atomic<bool> g_running{false};
void signal_handler(int sign){
    g_running=true;
}

int main()
{
    Config cfg;
    signal(SIGINT,signal_handler);
    signal(SIGTERM,signal_handler);

    ThreadPool pool(cfg.thread_num);
    EpollServer server(cfg.server_port,pool);

    atomic<bool> monitor_running{true};
    thread server_thread([&]() { 
        server.run(); 
    });
    thread mon_thread(monitor, ref(server), ref(monitor_running), cfg.monitor_interval);

    while (!g_running){
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    g_logger.log(LogLevel::INFO, "Shutting down...");

    // 停止服务和线程池
    server.stop();
    pool.shutdown();

    // 停止监控线程
    monitor_running = false;
    if (mon_thread.joinable()) mon_thread.join();

    // 异步日志 flush 并退出
    g_logger.shutdown();

    if (server_thread.joinable()) server_thread.join();

    g_logger.log(LogLevel::INFO, "Server exited gracefully.");
    return 0;
}