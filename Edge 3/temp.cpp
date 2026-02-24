#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstring>

using namespace std;

constexpr int PORT = 8080;
constexpr int MAX_EVENTS = 1024;
constexpr int BUFFER_SIZE = 4096;

//////////////////////////////////////////////////////
// 工具函数
//////////////////////////////////////////////////////

int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//////////////////////////////////////////////////////
// 线程池
//////////////////////////////////////////////////////

class ThreadPool {
public:
    ThreadPool(size_t n) : stop(false) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(this->mtx);
                        this->cv.wait(lock, [this] {
                            return stop || !tasks.empty();
                        });
                        if (stop && tasks.empty())
                            return;
                        task = move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    void enqueue(function<void()> task) {
        {
            unique_lock<mutex> lock(mtx);
            tasks.emplace(move(task));
        }
        cv.notify_one();
    }

    ~ThreadPool() {
        {
            unique_lock<mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();
        for (auto &w : workers)
            w.join();
    }

private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex mtx;
    condition_variable cv;
    bool stop;
};

//////////////////////////////////////////////////////
// 每个连接的缓冲区
//////////////////////////////////////////////////////

struct Connection {
    vector<char> buffer;
};

//////////////////////////////////////////////////////
// 主服务器
//////////////////////////////////////////////////////

int main() {

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    setNonBlocking(listen_fd);

    int epfd = epoll_create1(0);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    ThreadPool pool(thread::hardware_concurrency());

    unordered_map<int, Connection> connections;

    cout << "Server listening on port " << PORT << endl;

    epoll_event events[MAX_EVENTS];

    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; ++i) {

            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // 新连接
                int client_fd = accept(listen_fd, nullptr, nullptr);
                setNonBlocking(client_fd);

                epoll_event cev{};
                cev.events = EPOLLIN | EPOLLET;
                cev.data.fd = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);

                connections[client_fd] = Connection{};
            }
            else {
                // 数据读取
                while (true) {
                    char buf[BUFFER_SIZE];
                    ssize_t count = read(fd, buf, sizeof(buf));

                    if (count == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        close(fd);
                        connections.erase(fd);
                        break;
                    }
                    else if (count == 0) {
                        close(fd);
                        connections.erase(fd);
                        break;
                    }
                    else {
                        auto &conn = connections[fd];
                        conn.buffer.insert(conn.buffer.end(), buf, buf + count);

                        // 解析完整包
                        while (true) {
                            if (conn.buffer.size() < 4)
                                break;

                            uint32_t len;
                            memcpy(&len, conn.buffer.data(), 4);
                            len = ntohl(len);

                            if (conn.buffer.size() < 4 + len)
                                break;

                            string message(
                                conn.buffer.begin() + 4,
                                conn.buffer.begin() + 4 + len
                            );

                            // 提交线程池处理
                            pool.enqueue([fd, message]() {
                                string response = "OK: " + message;

                                uint32_t rlen = htonl(response.size());
                                vector<char> sendbuf(4 + response.size());
                                memcpy(sendbuf.data(), &rlen, 4);
                                memcpy(sendbuf.data() + 4, response.data(), response.size());

                                send(fd, sendbuf.data(), sendbuf.size(), 0);
                            });

                            // 删除已处理数据
                            conn.buffer.erase(
                                conn.buffer.begin(),
                                conn.buffer.begin() + 4 + len
                            );
                        }
                    }
                }
            }
        }
    }

    close(listen_fd);
    return 0;
}