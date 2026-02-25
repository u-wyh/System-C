#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <sstream>

using namespace std;

// 线程输出函数，保证日志不乱
mutex ioMutex;
void log_line(const string& msg){
    lock_guard<mutex> lock(ioMutex);
    cout << msg << endl;
}

// 设置非阻塞
int set_nonblocking(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 简单线程池
class ThreadPool{
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running;

public:
    ThreadPool(size_t threadNum): running(true){
        for(size_t i = 0; i < threadNum; i++){
            workers.emplace_back([this](){
                while(running){
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        condvar.wait(lock, [this](){
                            return !tasks.empty() || !running;
                        });
                        if(!running && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop();
                    }
                    try{
                        task();
                    }
                    catch(const exception& e){
                        log_line("[Worker] exception: " + string(e.what()));
                    }
                    catch(...){
                        log_line("[Worker] unknown exception");
                    }
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
        running = false;
        condvar.notify_all();
        for(auto& t : workers){
            if(t.joinable()) t.join();
        }
    }
};

// ET Reactor Server
class EpollETServer{
private:
    int listen_fd;
    int epfd;
    ThreadPool &pool;
    unordered_map<int, string> buffers;

public:
    EpollETServer(int port, ThreadPool &tp): pool(tp){
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
        listen(listen_fd, 128);

        set_nonblocking(listen_fd);

        epfd = epoll_create1(0);

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET; // ET 模式
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        log_line("[Server] listening on port " + to_string(port));
    }

    void run(){
        vector<epoll_event> events(1024);
        while(true){
            int n = epoll_wait(epfd, events.data(), events.size(), -1);
            if(n < 0){
                perror("epoll_wait");
                continue;
            }
            for(int i = 0; i < n; i++){
                int fd = events[i].data.fd;
                if(fd == listen_fd){
                    accept_clients();
                } else {
                    handle_client(fd);
                }
            }
        }
    }

    void accept_clients(){
        while(true){
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = accept(listen_fd, (sockaddr*)&client, &len);
            if(cfd < 0){
                if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("accept fail");
                continue;
            }
            set_nonblocking(cfd);

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET; // ET
            ev.data.fd = cfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

            log_line("[+] new client fd=" + to_string(cfd));
        }
    }

    void close_client(int fd){
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        buffers.erase(fd);
        log_line("[-] client close fd=" + to_string(fd));
    }

    void handle_client(int fd){
        char buffer[1024];
        while(true){  // ET 必须循环读取
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if(n > 0){
                buffers[fd].append(buffer, n);
                process_buffer(fd);
            } else if(n == 0){
                close_client(fd);
                break;
            } else {
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    break; // 数据已读完
                } else {
                    perror("read error");
                    close_client(fd);
                    break;
                }
            }
        }
    }

    void process_buffer(int fd){
        auto& buffer = buffers[fd];
        size_t pos;
        while((pos = buffer.find('\n')) != string::npos){
            string msg = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            log_line("[Reactor] fd=" + to_string(fd) + " received msg: " + msg);

            pool.submit([fd, msg](){
                stringstream ss;
                ss << this_thread::get_id();
                log_line("[Worker] thread=" + ss.str() + " processing fd=" + to_string(fd) + " msg=" + msg);
            });

            string echo = msg + "\n";
            write(fd, echo.c_str(), echo.size());
        }
    }
};

int main(){
    ThreadPool pool(4);
    EpollETServer server(8080, pool);
    server.run();
    return 0;
}