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

struct Config {
    int thread_num = 4;
    int server_port = 8080;
    string log_level = "INFO";

    bool load(const string &filename) {
        ifstream fin(filename);
        if (!fin.is_open()) return false;

        string line;
        while (getline(fin, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto pos = line.find('=');
            if (pos == string::npos) continue;

            string key = line.substr(0, pos);
            string val = line.substr(pos + 1);
            if (key == "thread_num") thread_num = stoi(val);
            else if (key == "server_port") server_port = stoi(val);
            else if (key == "log_level") log_level = val;
        }
        return true;
    }
};

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };
LogLevel g_log_level = LogLevel::INFO;
mutex ioMutex;

LogLevel str_to_level(const string &s) {
    if (s == "DEBUG") return LogLevel::DEBUG;
    if (s == "INFO") return LogLevel::INFO;
    if (s == "WARN") return LogLevel::WARN;
    if (s == "ERROR") return LogLevel::ERROR;
    return LogLevel::INFO;
}

string now_time() {
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return string(buf);
}

void log_line(LogLevel lvl, const string &msg) {
    if (lvl < g_log_level) return;
    lock_guard<mutex> lock(ioMutex);
    string lvl_str;
    switch (lvl) {
        case LogLevel::DEBUG: lvl_str = "DEBUG"; break;
        case LogLevel::INFO: lvl_str = "INFO"; break;
        case LogLevel::WARN: lvl_str = "WARN"; break;
        case LogLevel::ERROR: lvl_str = "ERROR"; break;
    }
    cout << "[" << now_time() << "]"
         << "[" << lvl_str << "] " << msg << endl;
}

class ThreadPool {
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running;

public:
    ThreadPool(size_t threadNum) : running(true) {
        for (size_t i = 0; i < threadNum; i++) {
            workers.emplace_back([this, i]() {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        condvar.wait(lock, [this]() {
                            return !tasks.empty() || !running;
                        });
                        if (!running && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop();
                    }

                    try {
                        task();
                    } catch (const exception &e) {
                        log_line(LogLevel::ERROR, "[Worker] exception: " + string(e.what()));
                    } catch (...) {
                        log_line(LogLevel::ERROR, "[Worker] unknown exception");
                    }
                }
            });
        }
    }

    ~ThreadPool() { shutdown(); }

    void submit(function<void()> task) {
        if (!running) throw runtime_error("ThreadPool stopped");
        {
            lock_guard<mutex> lock(queueMutex);
            tasks.push(move(task));
        }
        condvar.notify_one();
    }

    void shutdown() {
        running = false;
        condvar.notify_all();
        for (auto &t : workers)
            if (t.joinable()) t.join();
    }
};

class Service {
public:
    enum class State { INIT, RUNNING, STOPPING, STOPPED };

    Service(size_t threadNum) : state(State::INIT), pool(threadNum) {}

    ~Service() {
        if (get_state() == State::RUNNING || get_state() == State::STOPPING) {
            stop();
        }
    }

    void start() {
        lock_guard<mutex> lock(stateMutex);
        if (state != State::INIT) {
            log_line(LogLevel::ERROR, "Service cannot start, state=" + state_to_string(state));
            return;
        }
        state = State::RUNNING;
        log_line(LogLevel::INFO, "Service starting...");
    }

    void stop() {
        {
            lock_guard<mutex> lock(stateMutex);
            if (state == State::STOPPED) return;
            log_line(LogLevel::INFO, "Service stopping...");
            state = State::STOPPING;
        }
        pool.shutdown();
        lock_guard<mutex> lock(stateMutex);
        state = State::STOPPED;
        log_line(LogLevel::INFO, "Service stopped.");
    }

    State get_state() {
        lock_guard<mutex> lock(stateMutex);
        return state;
    }

    static string state_to_string(State s) {
        switch (s) {
            case State::INIT: return "INIT";
            case State::RUNNING: return "RUNNING";
            case State::STOPPING: return "STOPPING";
            case State::STOPPED: return "STOPPED";
            default: return "UNKNOWN";
        }
    }

    ThreadPool &get_pool() { return pool; }

private:
    mutex stateMutex;
    State state;
    ThreadPool pool;
};

Service *g_service = nullptr;

void signal_handler(int signal) {
    if (g_service) {
        log_line(LogLevel::WARN, "Signal received: " + to_string(signal));
        g_service->stop();
    }
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class EpollServer {
private:
    int listen_fd;
    int epfd;
    ThreadPool &pool;
    atomic<bool> running{true};
    unordered_map<int, string> buffers;

public:
    EpollServer(int port, ThreadPool &tp) : pool(tp) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        bind(listen_fd, (sockaddr *)&addr, sizeof(addr));
        listen(listen_fd, 1024);
        set_nonblocking(listen_fd);

        epfd = epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        log_line(LogLevel::INFO, "[Server] listening on port " + to_string(port));
    }

    void run() {
        vector<epoll_event> events(1024);
        while (running) {
            int n = epoll_wait(epfd, events.data(), events.size(), 500);
            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                if (fd == listen_fd) accept_client();
                else handle_client(fd);
            }
        }
        close(listen_fd);
        log_line(LogLevel::INFO, "[Server] stopped.");
    }

    void stop() { running = false; }

private:
    void accept_client() {
        while (true) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = accept(listen_fd, (sockaddr *)&client, &len);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("accept fail");
                continue;
            }
            set_nonblocking(cfd);

            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = cfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

            log_line(LogLevel::INFO, "[+] new client fd=" + to_string(cfd));
        }
    }

    void close_client(int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        buffers.erase(fd);
        log_line(LogLevel::INFO, "[-] client closed fd=" + to_string(fd));
    }

    void handle_client(int fd) {
        char buffer[1024];
        while (true) {
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                buffers[fd].append(buffer, n);
                process_buffer(fd);
            } else if (n == 0) {
                close_client(fd);
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("read error");
                close_client(fd);
                break;
            }
        }
    }

    void process_buffer(int fd) {
        auto &buffer = buffers[fd];
        size_t pos;
        while ((pos = buffer.find('\n')) != string::npos) {
            string msg = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            log_line(LogLevel::DEBUG, "[Reactor] fd=" + to_string(fd) + " received msg:" + msg);

            // 将业务任务提交到线程池
            pool.submit([fd, msg]() {
                stringstream ss;
                ss << this_thread::get_id();
                log_line(LogLevel::DEBUG, "[Worker] thread=" + ss.str() +
                                           " processing fd=" + to_string(fd) + " msg=" + msg);
            });

            string echo = msg + "\n";
            write(fd, echo.c_str(), echo.size());
        }
    }
};

int main() {
    Config cfg;
    if (!cfg.load("server_config.ini")) {
        log_line(LogLevel::WARN, "Failed to load config, using defaults.");
    }

    g_log_level = str_to_level(cfg.log_level);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    Service service(cfg.thread_num);
    g_service = &service;
    service.start();

    EpollServer server(cfg.server_port, service.get_pool());
    thread server_thread([&]() { server.run(); });

    while (service.get_state() == Service::State::RUNNING) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    server.stop();
    if (server_thread.joinable()) server_thread.join();

    log_line(LogLevel::INFO, "Server exiting gracefully.");
    return 0;
}