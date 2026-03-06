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
#include <sys/stat.h>
#include <nlohmann/json.hpp> 
#include <sys/wait.h>
#include <sys/types.h>

using json = nlohmann::json;
using namespace std;

struct Config{
    int thread_num=4;
    int server_port=8080;
    string log_file="server.log";
    int monitor_interval=1000; // ms
    int log_batch=256;
    int log_flush=50; // ms
};

enum class LogLevel{
    DEBUG=0,INFO=1,WARN=2,ERROR=3
};

class AsyncLogger{
private:
    vector<string> buffer1, buffer2;
    atomic<bool> swap_flag{false};
    mutex mtx;
    condition_variable cv;
    atomic<bool> running{true};
    thread worker;
    ofstream fout;
    atomic<LogLevel> level;
    size_t batch;
    int flush;

public:
    string now_time(){
        auto now = chrono::system_clock::now();
        time_t t = chrono::system_clock::to_time_t(now);
        tm tm_buf{};
        localtime_r(&t, &tm_buf);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return string(buf);
    }

    void process(){
        vector<string>* current;
        vector<string>* back;

        while(running || !buffer1.empty() || !buffer2.empty()){
            {
                unique_lock<mutex> lock(mtx);
                cv.wait_for(lock, chrono::milliseconds(flush), [this](){
                    return !buffer1.empty() || !buffer2.empty() || !running;
                });
                swap_flag = !swap_flag;
                if(swap_flag){
                    current = &buffer1;
                    back = &buffer2;
                } else {
                    current = &buffer2;
                    back = &buffer1;
                }
                current->swap(*back);
            }
            for(auto &line : *current){
                cout << line << endl;
                if(fout.is_open()){
                    fout << line << endl;
                }
            }
            current->clear();
        }
    }

    AsyncLogger(const string &filename, LogLevel lvl=LogLevel::INFO, size_t batch_size=256, int flush_ms=50)
        : batch(batch_size), flush(flush_ms), level(lvl) 
    {
        fout.open(filename, ios::app);
        buffer1.reserve(batch*2);
        buffer2.reserve(batch*2);
        worker = thread(&AsyncLogger::process, this);
    }

    ~AsyncLogger(){
        shutdown();
    }

    void shutdown(){
        running = false;
        cv.notify_all();
        if(worker.joinable()) worker.join();
        if(fout.is_open()) fout.close();
    }

    void log(LogLevel lvl, const string& msg){
        if(lvl < level.load()) return;
        string lvl_str;
        switch(lvl){
            case LogLevel::DEBUG: lvl_str="DEBUG"; break;
            case LogLevel::INFO: lvl_str="INFO"; break;
            case LogLevel::WARN: lvl_str="WARN"; break;
            case LogLevel::ERROR: lvl_str="ERROR"; break;
        }
        string line = "[" + now_time() + "][" + lvl_str + "] " + msg;
        {
            lock_guard<mutex> lock(mtx);
            if(swap_flag) buffer1.push_back(move(line));
            else buffer2.push_back(move(line));
        }
        cv.notify_one();
    }

    void set_level(LogLevel lvl){
        level.store(lvl);
        log(LogLevel::INFO, "[AsyncLogger] log level updated");
    }
};

AsyncLogger g_logger("server.log", LogLevel::INFO);

class ThreadPool {
    
private:
    vector<thread> workers;              // 工作线程列表
    queue<function<void()>> tasks;       // 任务队列
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running{true};          // 总开关
    atomic<size_t> target_thread_num;    // 配置的目标线程数

public:
    ThreadPool(size_t threadNum) : target_thread_num(threadNum) {
        resize(threadNum);
    }

    ~ThreadPool() { shutdown(); }

    // 提交任务
    void submit(function<void()> task){
        {
            lock_guard<mutex> lock(queueMutex);
            tasks.push(move(task));
        }
        condvar.notify_one();
    }

    // 动态调整线程数
    void resize(size_t newSize){
        target_thread_num = newSize;

        lock_guard<mutex> lock(queueMutex);

        // 增加线程
        if(newSize > workers.size()){
            for(size_t i = workers.size(); i < newSize; ++i){
                workers.emplace_back([this](){ worker_loop(); });
            }
            g_logger.log(LogLevel::INFO, "[ThreadPool] resized to "+to_string(newSize)+" threads");
        }
    }

    // 停止线程池
    void shutdown(){
        running = false;
        condvar.notify_all();
        for(auto &t : workers) if(t.joinable()) t.join();
        workers.clear();
    }

    void worker_loop(){
        while(true){
            function<void()> task;

            {
                unique_lock<mutex> lock(queueMutex);
                // 等待任务或线程池关闭
                condvar.wait(lock, [this](){
                    return !tasks.empty() || !running;
                });

                // 线程池关闭并且任务队列为空 → 退出
                if(!running && tasks.empty()) return;

                // 线程数量超过目标值 → 空闲线程退出
                if(workers.size() > target_thread_num) return;

                if(!tasks.empty()){
                    task = move(tasks.front());
                    tasks.pop();
                }
            }

            // 执行任务
            if(task){
                try {
                    task();
                } catch(const exception& e){
                    g_logger.log(LogLevel::ERROR, "[Worker] "+string(e.what()));
                } catch(...){
                    g_logger.log(LogLevel::ERROR, "[Worker] unknown exception");
                }
            }
        }
    }
};

int set_nonblocking(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class EpollServer{
private:
    int listen_fd;
    int epfd;
    ThreadPool &pool;
    atomic<bool> running{true};
    unordered_map<int,string> buffers;
    atomic<int> tps{0};
    atomic<int64_t> total_tps{0};

public:
    EpollServer(int port, ThreadPool &tp) : pool(tp){
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
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
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        g_logger.log(LogLevel::INFO, "[Server] listening on port "+to_string(port));
    }

    void run(){
        vector<epoll_event> events(1024);
        while(running){
            int n = epoll_wait(epfd, events.data(), events.size(), 500);
            for(int i=0;i<n;i++){
                int fd = events[i].data.fd;
                if(fd == listen_fd) accept_client();
                else handle_client(fd);
            }
        }
        close(listen_fd);
        g_logger.log(LogLevel::INFO, "[Server] stopped.");
    }

    void stop(){ running=false; }

    void accept_client(){
        while(true){
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = accept(listen_fd, (sockaddr*)&client, &len);
            if(cfd < 0){
                if(errno == EAGAIN) break;
                perror("accept fail");
                continue;
            }
            set_nonblocking(cfd);
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = cfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
        }
    }

    void close_client(int fd){
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        buffers.erase(fd);
    }

    void handle_client(int fd){
        char buffer[1024];
        while(true){
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if(n > 0){
                buffers[fd].append(buffer, n);
                process_buffer(fd);
            } else if(n == 0){
                close_client(fd);
                break;
            } else {
                if(errno==EAGAIN||errno==EWOULDBLOCK) break;
                perror("read error");
                close_client(fd);
                break;
            }
        }
    }

    void process_buffer(int fd){
        auto &buffer = buffers[fd];
        size_t pos;
        while((pos = buffer.find('\n')) != string::npos){
            string msg = buffer.substr(0, pos);
            buffer.erase(0, pos+1);
            tps++;
            total_tps++;

            pool.submit([fd, msg](){
                stringstream ss;
                ss << this_thread::get_id();
                g_logger.log(LogLevel::DEBUG, "[Worker] thread=" + ss.str() + " processing fd=" + to_string(fd) + " msg=" + msg);
            });

            string echo = msg + "\n";
            write(fd, echo.c_str(), echo.size());
        }
    }

    int get_tps(){
        int t = tps.load();
        tps = 0;
        return t;
    }

    int64_t get_total_tps(){ return total_tps.load(); }
};

void monitor(EpollServer &server, atomic<bool>& running, int interval){
    uint64_t lastTotalTime = 0;
    auto lastCheck = chrono::steady_clock::now();

    while(running){
        this_thread::sleep_for(chrono::milliseconds(interval));

        // CPU
        ifstream fstat("/proc/self/stat");
        string tmp;
        uint64_t utime=0, stime=0;
        for(int i=0;i<13;i++) fstat >> tmp;
        fstat >> utime >> stime;
        uint64_t totalTime = utime + stime;

        auto now = chrono::steady_clock::now();
        double elapsedSec = chrono::duration<double>(now - lastCheck).count();
        double cpuUsage = 0.0;
        if(lastTotalTime != 0 && elapsedSec>0)
            cpuUsage = (double)(totalTime - lastTotalTime) / sysconf(_SC_CLK_TCK) / elapsedSec * 100.0;
        lastTotalTime = totalTime;
        lastCheck = now;

        // Mem
        ifstream fstatus("/proc/self/status");
        string line; size_t memKB=0;
        while(getline(fstatus, line)){
            if(line.find("VmRSS:") != string::npos){
                stringstream ss(line);
                string key; ss >> key >> memKB;
            }
        }

        // TPS
        uint64_t tps = server.get_tps();

        g_logger.log(LogLevel::INFO, "[Monitor] CPU=" + to_string(cpuUsage) +
                                      "% Mem=" + to_string(memKB/1024.0) + "MB TPS=" + to_string(tps));
    }
}

class ConfigManager {
private:
    string filename;
    Config* cfg;
    atomic<bool> running{true};
    AsyncLogger* logger;
    ThreadPool* pool;
    time_t last_mtime{0};

public:
    ConfigManager(const string &file, Config* c, AsyncLogger* l, ThreadPool* p)
        : filename(file), cfg(c), logger(l), pool(p) {}

    void stop(){ running=false; }

    void watch_loop() {
        while(running){
            this_thread::sleep_for(chrono::milliseconds(cfg->monitor_interval));

            struct stat st;
            if(stat(filename.c_str(), &st)==0){
                if(st.st_mtime != last_mtime){
                    last_mtime = st.st_mtime;
                    update_config();
                }
            }
        }
    }

    void update_config(){
        ifstream f(filename);
        if(!f.is_open()) return;
        json j;
        try{
            f >> j;
        } catch(...){
            g_logger.log(LogLevel::WARN, "[ConfigManager] failed to parse config file");
            return;
        }

        // 更新线程池
        if(j.contains("thread_num")){
            int newThreads = j["thread_num"];
            if(newThreads != cfg->thread_num){
                g_logger.log(LogLevel::INFO, "[ConfigManager] updating thread_num: " +
                                             to_string(cfg->thread_num) + " -> " + to_string(newThreads));
                pool->resize(newThreads);
                cfg->thread_num = newThreads;
            }
        }

        // 更新日志等级
        if(j.contains("log_level")){
            string lvl = j["log_level"];
            LogLevel newLvl = LogLevel::INFO;
            if(lvl=="DEBUG") newLvl = LogLevel::DEBUG;
            else if(lvl=="INFO") newLvl = LogLevel::INFO;
            else if(lvl=="WARN") newLvl = LogLevel::WARN;
            else if(lvl=="ERROR") newLvl = LogLevel::ERROR;

            logger->set_level(newLvl);
        }
    }
};

atomic<bool> g_running{false};
void signal_handler(int){
    g_running = true;
}

int run_server(){

    Config cfg;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    ThreadPool pool(cfg.thread_num);
    EpollServer server(cfg.server_port, pool);

    atomic<bool> monitor_running{true};

    thread server_thread([&](){
        server.run();
    });

    thread mon_thread(
        monitor,
        ref(server),
        ref(monitor_running),
        cfg.monitor_interval
    );

    ConfigManager cfgMgr("config.json", &cfg, &g_logger, &pool);

    thread cfg_thread([&](){
        cfgMgr.watch_loop();
    });

    while(!g_running){
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    g_logger.log(LogLevel::INFO, "Shutting down...");
    cout<<flush;

    // 停止服务器
    server.stop();

    // 关闭线程池
    pool.shutdown();

    // 关闭监控线程
    monitor_running = false;
    if(mon_thread.joinable())
        mon_thread.join();

    // 关闭配置监听
    cfgMgr.stop();
    if(cfg_thread.joinable())
        cfg_thread.join();

    // 停止日志
    g_logger.shutdown();

    if(server_thread.joinable())
        server_thread.join();

    cout << "[Server] Total requests processed: "
         << server.get_total_tps() << endl;

    g_logger.log(
        LogLevel::INFO,
        "[Server] Total requests processed: "
        + to_string(server.get_total_tps())
    );

    g_logger.log(LogLevel::INFO, "Server exited gracefully.");

    return 0;
}

int main(){

    while(true){

        pid_t pid = fork();

        if(pid < 0){
            perror("fork failed");
            return 1;
        }

        // 子进程
        if(pid == 0){

            cout << "[Watchdog] starting server..." << endl;

            int code = run_server();

            exit(code);
        }

        // 父进程
        int status;
        cout<<pid<<endl;
        waitpid(pid, &status, 0);

        if(WIFEXITED(status)){

            int code = WEXITSTATUS(status);

            cout << "[Watchdog] server exited with code "
                 << code << endl;

            // 正常退出就不重启
            if(code == 0){
                cout << "[Watchdog] normal shutdown" << endl;
                break;
            }
        }

        if(WIFSIGNALED(status)){

            cout << "[Watchdog] server crashed by signal "
                 << WTERMSIG(status)
                 << endl;
        }

        cout << "[Watchdog] restarting server in 2 seconds..."
             << endl;

        sleep(2);
    }

    return 0;
}