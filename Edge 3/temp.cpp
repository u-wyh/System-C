#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace std;

constexpr int PORT = 8080;
constexpr int MAX_EVENTS = 1024;
constexpr int WORKER_NUM = 4;   // 建议 = CPU 核数

atomic<bool> g_running{true};
atomic<long long> g_total_requests{0};
atomic<long long> g_active_connections{0};

void signal_handler(int)
{
    g_running = false;
}

void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class Worker
{
public:
    Worker()
    {
        epfd = epoll_create1(0);
    }

    void add_fd(int fd)
    {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

        g_active_connections++;
    }

    void run()
    {
        epoll_event events[MAX_EVENTS];

        while (g_running)
        {
            int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
            for (int i = 0; i < n; i++)
            {
                int fd = events[i].data.fd;
                handle_read(fd);
            }
        }

        close(epfd);
    }

private:
    int epfd;

    void handle_read(int fd)
    {
        char buf[4096];

        while (true)
        {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);

            if (n > 0)
            {
                g_total_requests++;
                send(fd, buf, n, 0);
            }
            else if (n == 0)
            {
                close(fd);
                g_active_connections--;
                break;
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;

                close(fd);
                g_active_connections--;
                break;
            }
        }
    }
};

int main()
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    set_nonblock(listenfd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, 1024);

    cout << "[Server] listening on port " << PORT << endl;

    vector<unique_ptr<Worker>> workers;
    vector<thread> threads;

    for (int i = 0; i < WORKER_NUM; i++)
    {
        workers.emplace_back(new Worker());
    }

    for (int i = 0; i < WORKER_NUM; i++)
    {
        threads.emplace_back(&Worker::run, workers[i].get());
    }

    // 监控线程
    thread monitor([](){
        long long last_total = 0;
        int sec = 0;

        while (g_running)
        {
            this_thread::sleep_for(chrono::seconds(1));

            long long now = g_total_requests.load();
            long long tps = now - last_total;

            cout << "[Monitor] "
                 << ++sec
                 << "s total=" << now
                 << " TPS=" << tps
                 << " active_conn=" << g_active_connections.load()
                 << endl;

            last_total = now;
        }
    });

    int idx = 0;

    while (g_running)
    {
        sockaddr_in client{};
        socklen_t len = sizeof(client);

        int connfd = accept(listenfd, (sockaddr*)&client, &len);

        if (connfd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                this_thread::sleep_for(chrono::milliseconds(1));
                continue;
            }
            continue;
        }

        set_nonblock(connfd);

        workers[idx]->add_fd(connfd);
        idx = (idx + 1) % WORKER_NUM;
    }

    cout << "Shutting down..." << endl;

    close(listenfd);

    for (auto &t : threads)
        if (t.joinable())
            t.join();

    if (monitor.joinable())
        monitor.join();

    cout << "Server exited gracefully." << endl;

    return 0;
}
