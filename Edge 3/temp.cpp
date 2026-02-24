#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include<unordered_map>

using namespace std;

atomic<bool> g_running{true};
atomic<int> active_connections{0};

void signal_handler(int) { g_running = false; }

// 设置非阻塞
int set_nonblocking(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 客户端封装
struct Client {
    int fd;
    atomic<bool> closed{false};

    Client(int f) : fd(f) {}

    void close_fd() {
        bool expected = false;
        if (closed.compare_exchange_strong(expected, true)) {
            ::close(fd);
            active_connections--;
            cout << "[Client] fd " << fd << " closed." << endl;
        }
    }

    ~Client() {
        close_fd();
    }
};

// 简单线程池
class ThreadPool {
public:
    ThreadPool(size_t n) : stop(false) {
        for(size_t i=0;i<n;i++){
            workers.emplace_back([this]{
                while(true){
                    shared_ptr<Client> client;
                    {
                        unique_lock<mutex> lock(mtx);
                        cv.wait(lock, [this]{ return stop || !tasks.empty(); });
                        if(stop && tasks.empty()) return;
                        client = tasks.front();
                        tasks.pop();
                    }

                    try {
                        handle_client(client);
                    } catch (const bad_alloc& e) {
                        cout << "[ThreadPool] bad_alloc exception for fd " << client->fd << endl;
                    } catch (const exception& e) {
                        cout << "[ThreadPool] exception: " << e.what() << " fd " << client->fd << endl;
                    } catch (...) {
                        cout << "[ThreadPool] unknown exception fd " << client->fd << endl;
                    }
                }
            });
        }
    }

    void enqueue(shared_ptr<Client> client){
        {
            lock_guard<mutex> lock(mtx);
            tasks.push(client);
        }
        cv.notify_one();
    }

    void shutdown(){
        {
            lock_guard<mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();
        for(auto &t : workers) t.join();
    }

private:
    vector<thread> workers;
    queue<shared_ptr<Client>> tasks;
    mutex mtx;
    condition_variable cv;
    bool stop;

    static void handle_client(shared_ptr<Client> client){
        char buffer[1024];
        int simulate = rand() % 10; // 模拟不同异常情况
        if(simulate == 0) throw runtime_error("Simulated thread exception");
        if(simulate == 1) throw bad_alloc();

        while(true){
            ssize_t n = recv(client->fd, buffer, sizeof(buffer), 0);
            if(n > 0){
                // 模拟偶尔写异常
                if(rand()%20==0) {
                    client->close_fd();
                    break;
                }
                send(client->fd, buffer, n, 0);
            } else if(n == 0){
                client->close_fd();
                break;
            } else {
                if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                client->close_fd();
                break;
            }
        }
    }
};

int main(){
    srand(time(nullptr));
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int port = 8080;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(listen_fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    int epfd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    ThreadPool pool(thread::hardware_concurrency());
    vector<epoll_event> events(128);

    unordered_map<int, shared_ptr<Client>> clients;

    cout << "[Server] Listening on port " << port << endl;

    while(g_running){
        int n = epoll_wait(epfd, events.data(), events.size(), 1000);
        if(n<0 && errno!=EINTR) continue;

        for(int i=0;i<n;i++){
            int fd = events[i].data.fd;
            if(fd == listen_fd){
                while(true){
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int conn = accept(listen_fd, (sockaddr*)&client_addr, &len);
                    if(conn < 0){
                        if(errno==EAGAIN || errno==EWOULDBLOCK) break;
                        continue;
                    }
                    set_nonblocking(conn);
                    auto client = make_shared<Client>(conn);
                    clients[conn] = client;
                    active_connections++;
                }
            } else {
                auto it = clients.find(fd);
                if(it!=clients.end()){
                    pool.enqueue(it->second);
                }
            }
        }
    }

    cout << "[Server] Shutting down..." << endl;
    close(listen_fd);
    pool.shutdown();
    close(epfd);

    for(auto &p : clients) p.second->close_fd();

    return 0;
}