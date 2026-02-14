#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

#include <vector>
#include <iostream>
#include <unordered_map>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <memory>
#include <chrono>

using namespace std;

/* ===================== 全局统计 ===================== */

atomic<long long> g_total_requests{0};
atomic<int> g_active_connections{0};
atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

/* ===================== 工具函数 ===================== */

int set_nonblocking(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ===================== 线程池 ===================== */

class ThreadPool {
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running;

public:
    ThreadPool(size_t n):running(true){
        for(size_t i=0;i<n;i++){
            workers.emplace_back([this](){
                while(true){
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        condvar.wait(lock,[this](){ return !tasks.empty() || !running; });
                        if(!running && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    void submit(function<void()> task){
        if(!running) return;
        {
            lock_guard<mutex> lock(queueMutex);
            tasks.push(move(task));
        }
        condvar.notify_one();
    }

    void shutdown(){
        running = false;
        condvar.notify_all();
        for(auto& t:workers)
            if(t.joinable()) t.join();
    }

    ~ThreadPool(){ shutdown(); }
};

/* ===================== 连接结构 ===================== */

struct Connection {
    int fd;
    string readBuffer;
    string writeBuffer;
};

/* ===================== 服务器 ===================== */

class EpollServer {
private:
    int listen_fd;
    int epfd;
    ThreadPool& pool;
    unordered_map<int, shared_ptr<Connection>> connections;
    thread monitorThread;

public:
    EpollServer(int port, ThreadPool& tp):pool(tp){
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
        listen(listen_fd, 1024);
        set_nonblocking(listen_fd);

        epfd = epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        cout << "[Server] listening on port " << port << endl;
    }

    void run(){

        // 性能监控线程
        monitorThread = thread([&](){
            long long last_total = 0;
            while(g_running){
                this_thread::sleep_for(chrono::seconds(1));
                long long cur_total = g_total_requests.load();
                int active = g_active_connections.load();
                cout << "[Monitor] total_requests=" << cur_total 
                     << " TPS=" << (cur_total - last_total)
                     << " active_conn=" << active << endl;
                last_total = cur_total;
            }
        });

        vector<epoll_event> events(1024);

        while(g_running){
            int n = epoll_wait(epfd, events.data(), events.size(), 1000);
            if(n < 0){
                if(errno == EINTR) continue;
                break;
            }

            for(int i=0;i<n;i++){
                int fd = events[i].data.fd;

                if(fd == listen_fd){
                    accept_client();
                } else {
                    if(events[i].events & EPOLLIN)
                        handle_read(fd);
                    if(events[i].events & EPOLLOUT)
                        handle_write(fd);
                }
            }
        }

        shutdown();
    }

private:

    void shutdown(){
        cout << "[Server] shutting down..." << endl;

        for(auto& [fd,_]:connections) close(fd);
        close(listen_fd);
        close(epfd);

        pool.shutdown();

        if(monitorThread.joinable())
            monitorThread.join();

        cout << "[Server] stopped." << endl;
    }

    void accept_client(){
        while(true){
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = accept(listen_fd,(sockaddr*)&client,&len);
            if(cfd < 0){
                if(errno == EAGAIN) break;
                continue;
            }

            set_nonblocking(cfd);

            auto conn = make_shared<Connection>();
            conn->fd = cfd;
            connections[cfd] = conn;
            g_active_connections++;

            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = cfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
        }
    }

    void handle_read(int fd){
        char buffer[1024];
        auto conn = connections[fd];

        while(true){
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if(n > 0){
                conn->readBuffer.append(buffer, n);

                // 业务处理交给线程池
                pool.submit([this, conn](){
                    string response = conn->readBuffer;
                    conn->writeBuffer += response;
                    conn->readBuffer.clear();
                    modify_to_write(conn->fd);
                    g_total_requests++;
                });
            }
            else if(n == 0){
                close_client(fd);
                break;
            }
            else{
                if(errno == EAGAIN) break;
                close_client(fd);
                break;
            }
        }
    }

    void handle_write(int fd){
        auto conn = connections[fd];

        while(!conn->writeBuffer.empty()){
            ssize_t n = write(fd, conn->writeBuffer.data(), conn->writeBuffer.size());
            if(n > 0){
                conn->writeBuffer.erase(0,n);
            }
            else{
                if(errno == EAGAIN) break;
                close_client(fd);
                return;
            }
        }

        if(conn->writeBuffer.empty())
            modify_to_read(fd);
    }

    void modify_to_write(int fd){
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }

    void modify_to_read(int fd){
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }

    void close_client(int fd){
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        connections.erase(fd);
        g_active_connections--;
    }
};

int main(){
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    ThreadPool pool(4);
    EpollServer server(8080, pool);
    server.run();
    return 0;
}
