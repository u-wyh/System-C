#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cerrno>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using namespace std;

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

struct Config {
    int thread_num = 4;
    int server_port = 8080;
    string log_file = "server.log";
    int monitor_interval = 1000;
    int log_batch = 256;
    int log_flush = 50;
    double cpu_warn = 100.0;
    double cpu_error = 180.0;
    double mem_warn = 100.0;
    double mem_error = 200.0;
    int tps_warn = 20000;
    int tps_error = 40000;
    string log_level = "INFO";
};

LogLevel parse_log_level(const string& text) {
    if (text == "DEBUG") return LogLevel::DEBUG;
    if (text == "WARN") return LogLevel::WARN;
    if (text == "ERROR") return LogLevel::ERROR;
    return LogLevel::INFO;
}

bool load_config_file(const string& path, Config& cfg) {
    ifstream f(path);
    if (!f.is_open()) {
        return false;
    }

    json j;
    try {
        f >> j;
    } catch (...) {
        return false;
    }

    if (j.contains("thread_num")) cfg.thread_num = j["thread_num"];
    if (j.contains("server_port")) cfg.server_port = j["server_port"];
    if (j.contains("log_file")) cfg.log_file = j["log_file"];
    if (j.contains("monitor_interval")) cfg.monitor_interval = j["monitor_interval"];
    if (j.contains("log_batch")) cfg.log_batch = j["log_batch"];
    if (j.contains("log_flush")) cfg.log_flush = j["log_flush"];
    if (j.contains("cpu_warn")) cfg.cpu_warn = j["cpu_warn"];
    if (j.contains("cpu_error")) cfg.cpu_error = j["cpu_error"];
    if (j.contains("mem_warn")) cfg.mem_warn = j["mem_warn"];
    if (j.contains("mem_error")) cfg.mem_error = j["mem_error"];
    if (j.contains("tps_warn")) cfg.tps_warn = j["tps_warn"];
    if (j.contains("tps_error")) cfg.tps_error = j["tps_error"];
    if (j.contains("log_level")) cfg.log_level = j["log_level"];
    return true;
}

class AsyncLogger {
private:
    mutex mtx;
    atomic<bool> running{true};
    ofstream fout;
    string filename;
    atomic<LogLevel> level;

public:
    AsyncLogger(const string& file_name, LogLevel lvl, size_t batch_size, int flush_interval_ms)
        : filename(file_name), level(lvl) {
        (void)batch_size;
        (void)flush_interval_ms;
        fout.open(filename, ios::app);
    }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    ~AsyncLogger() {
        shutdown();
    }

    static string now_time() {
        auto now = chrono::system_clock::now();
        time_t t = chrono::system_clock::to_time_t(now);
        tm tm_buf{};
        localtime_r(&t, &tm_buf);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return string(buf);
    }

    void shutdown() {
        if (!running.exchange(false)) {
            return;
        }
        if (fout.is_open()) {
            fout.close();
        }
    }

    void log(LogLevel lvl, const string& msg) {
        if (lvl < level.load() || !running) {
            return;
        }

        const char* level_text = "INFO";
        switch (lvl) {
            case LogLevel::DEBUG: level_text = "DEBUG"; break;
            case LogLevel::INFO: level_text = "INFO"; break;
            case LogLevel::WARN: level_text = "WARN"; break;
            case LogLevel::ERROR: level_text = "ERROR"; break;
        }

        string line = "[" + now_time() + "][" + level_text + "] " + msg;
        lock_guard<mutex> lock(mtx);
        cout << line << "\n";
        if (fout.is_open()) {
            fout << line << "\n";
            fout.flush();
        }
    }

    void set_level(LogLevel lvl) {
        level.store(lvl);
        log(LogLevel::INFO, "[AsyncLogger] log level updated");
    }
};

static AsyncLogger* g_logger = nullptr;

inline void glog(LogLevel lvl, const string& msg) {
    if (g_logger) {
        g_logger->log(lvl, msg);
    }
}

class ThreadPool {
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condvar;
    atomic<bool> running{true};
    atomic<size_t> target_thread_num;

public:
    explicit ThreadPool(size_t n) : target_thread_num(n) {
        resize(n);
    }

    ~ThreadPool() {
        shutdown();
    }

    void submit(function<void()> task) {
        lock_guard<mutex> lock(queue_mutex);
        if (!running) {
            return;
        }
        tasks.push(move(task));
        condvar.notify_one();
    }

    void resize(size_t new_size) {
        target_thread_num = new_size;
        lock_guard<mutex> lock(queue_mutex);
        for (size_t i = workers.size(); i < new_size; ++i) {
            workers.emplace_back([this]() { worker_loop(); });
        }
        glog(LogLevel::INFO, "[ThreadPool] resized to " + to_string(new_size) + " threads");
    }

    void shutdown() {
        {
            lock_guard<mutex> lock(queue_mutex);
            if (!running) {
                return;
            }
            running = false;
        }
        condvar.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }

    void worker_loop() {
        while (true) {
            function<void()> task;
            {
                unique_lock<mutex> lock(queue_mutex);
                condvar.wait(lock, [this]() {
                    return !tasks.empty() || !running;
                });
                if (!running && tasks.empty()) {
                    return;
                }
                if (workers.size() > target_thread_num && tasks.empty()) {
                    return;
                }
                if (!tasks.empty()) {
                    task = move(tasks.front());
                    tasks.pop();
                }
            }
            if (task) {
                try {
                    task();
                } catch (const exception& e) {
                    glog(LogLevel::ERROR, "[Worker] " + string(e.what()));
                } catch (...) {
                    glog(LogLevel::ERROR, "[Worker] unknown exception");
                }
            }
        }
    }
};

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class EpollServer {
private:
    int listen_fd = -1;
    int epfd = -1;
    ThreadPool& pool;
    atomic<bool> running{true};
    mutex buffers_mtx;
    unordered_map<int, string> buffers;
    atomic<int> tps{0};
    atomic<int64_t> total_tps{0};

    bool send_line(int fd, const string& out) {
        ssize_t total = static_cast<ssize_t>(out.size());
        ssize_t sent = 0;
        while (sent < total) {
            ssize_t n = send(fd, out.c_str() + sent, total - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                this_thread::yield();
                continue;
            }
            return false;
        }
        return true;
    }

public:
    EpollServer(int port, ThreadPool& tp) : pool(tp) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(listen_fd, 512);
        set_nonblocking(listen_fd);

        epfd = epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        glog(LogLevel::INFO, "[Server] listening on port " + to_string(port));
    }

    void run() {
        vector<epoll_event> events(1024);
        while (running) {
            int n = epoll_wait(epfd, events.data(), static_cast<int>(events.size()), 500);
            if (n < 0) {
                if (errno == EINTR) {
                    if (!running) {
                        break;
                    }
                    continue;
                }
                if (running) {
                    perror("epoll_wait");
                }
                break;
            }

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == listen_fd) {
                    accept_client();
                } else {
                    handle_client(fd);
                }
            }
        }

        {
            lock_guard<mutex> lock(buffers_mtx);
            for (auto& kv : buffers) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, kv.first, nullptr);
                close(kv.first);
            }
            buffers.clear();
        }

        if (listen_fd >= 0) {
            close(listen_fd);
            listen_fd = -1;
        }
        if (epfd >= 0) {
            close(epfd);
            epfd = -1;
        }
        glog(LogLevel::INFO, "[Server] stopped.");
    }

    void stop() {
        running = false;
    }

    void accept_client() {
        while (true) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client), &len);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                continue;
            }
            set_nonblocking(cfd);
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = cfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
        }
    }

    void close_client(int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        lock_guard<mutex> lock(buffers_mtx);
        buffers.erase(fd);
    }

    void handle_client(int fd) {
        char buf[4096];
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                {
                    lock_guard<mutex> lock(buffers_mtx);
                    buffers[fd].append(buf, n);
                }
                process_buffer(fd);
            } else if (n == 0) {
                close_client(fd);
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                close_client(fd);
                break;
            }
        }
    }

    void process_buffer(int fd) {
        while (true) {
            string msg;
            {
                lock_guard<mutex> lock(buffers_mtx);
                auto it = buffers.find(fd);
                if (it == buffers.end()) {
                    break;
                }
                string& buffer = it->second;
                size_t pos = buffer.find('\n');
                if (pos == string::npos) {
                    break;
                }
                msg = buffer.substr(0, pos);
                if (!msg.empty() && msg.back() == '\r') {
                    msg.pop_back();
                }
                buffer.erase(0, pos + 1);
            }

            if (msg.empty()) {
                continue;
            }

            tps++;
            total_tps++;

            pool.submit([fd, msg]() {
                stringstream ss;
                ss << this_thread::get_id();
                glog(LogLevel::DEBUG, "[Worker] thread=" + ss.str() + " fd=" + to_string(fd) + " msg=" + msg);
            });

            string echo = msg + "\n";
            if (!send_line(fd, echo)) {
                close_client(fd);
                break;
            }
        }
    }

    int get_tps() {
        return tps.exchange(0);
    }

    int64_t get_total_tps() {
        return total_tps.load();
    }
};

void monitor(EpollServer& server, atomic<bool>& running, Config& cfg) {
    uint64_t last_total_time = 0;
    auto last_check = chrono::steady_clock::now();

    while (running) {
        this_thread::sleep_for(chrono::milliseconds(cfg.monitor_interval));
        if (!running) {
            break;
        }

        ifstream fstat("/proc/self/stat");
        string tmp;
        uint64_t utime = 0;
        uint64_t stime = 0;
        for (int i = 0; i < 13; ++i) {
            fstat >> tmp;
        }
        fstat >> utime >> stime;
        uint64_t total_time = utime + stime;

        auto now = chrono::steady_clock::now();
        double elapsed_sec = chrono::duration<double>(now - last_check).count();
        double cpu_usage = 0.0;
        if (last_total_time != 0 && elapsed_sec > 0) {
            cpu_usage = static_cast<double>(total_time - last_total_time) /
                        sysconf(_SC_CLK_TCK) / elapsed_sec * 100.0;
        }
        last_total_time = total_time;
        last_check = now;

        ifstream fstatus("/proc/self/status");
        string line;
        size_t mem_kb = 0;
        while (getline(fstatus, line)) {
            if (line.find("VmRSS:") != string::npos) {
                stringstream ss(line);
                string key;
                ss >> key >> mem_kb;
            }
        }

        int cur_tps = server.get_tps();
        {
            stringstream ss;
            ss << "[Monitor] CPU=" << cpu_usage
               << "% Mem=" << mem_kb / 1024.0
               << "MB TPS=" << cur_tps;
            glog(LogLevel::INFO, ss.str());
        }

        if (cpu_usage >= cfg.cpu_error) {
            glog(LogLevel::ERROR, "[ALARM] CPU too high: " + to_string(cpu_usage) + "%");
        } else if (cpu_usage >= cfg.cpu_warn) {
            glog(LogLevel::WARN, "[ALARM] CPU warning: " + to_string(cpu_usage) + "%");
        }

        double mem_mb = mem_kb / 1024.0;
        if (mem_mb >= cfg.mem_error) {
            glog(LogLevel::ERROR, "[ALARM] Mem too high: " + to_string(mem_mb) + "MB");
        } else if (mem_mb >= cfg.mem_warn) {
            glog(LogLevel::WARN, "[ALARM] Mem warning: " + to_string(mem_mb) + "MB");
        }

        if (cur_tps >= cfg.tps_error) {
            glog(LogLevel::ERROR, "[ALARM] TPS too high: " + to_string(cur_tps));
        } else if (cur_tps >= cfg.tps_warn) {
            glog(LogLevel::WARN, "[ALARM] TPS warning: " + to_string(cur_tps));
        }
    }
}

class ConfigManager {
private:
    string filename;
    Config* cfg;
    ThreadPool* pool;
    atomic<bool> running{true};
    time_t last_mtime{0};

public:
    ConfigManager(const string& file, Config* config, ThreadPool* thread_pool)
        : filename(file), cfg(config), pool(thread_pool) {}

    void stop() {
        running = false;
    }

    void watch_loop() {
        while (running) {
            this_thread::sleep_for(chrono::milliseconds(cfg->monitor_interval));
            if (!running) {
                break;
            }
            struct stat st{};
            if (stat(filename.c_str(), &st) == 0 && st.st_mtime != last_mtime) {
                last_mtime = st.st_mtime;
                update_config();
            }
        }
    }

    void update_config() {
        Config new_cfg = *cfg;
        if (!load_config_file(filename, new_cfg)) {
            glog(LogLevel::WARN, "[ConfigManager] failed to parse config");
            return;
        }

        if (new_cfg.thread_num != cfg->thread_num) {
            glog(LogLevel::INFO, "[ConfigManager] thread_num: " +
                                  to_string(cfg->thread_num) + " -> " + to_string(new_cfg.thread_num));
            pool->resize(new_cfg.thread_num);
            cfg->thread_num = new_cfg.thread_num;
        }

        cfg->cpu_warn = new_cfg.cpu_warn;
        cfg->cpu_error = new_cfg.cpu_error;
        cfg->mem_warn = new_cfg.mem_warn;
        cfg->mem_error = new_cfg.mem_error;
        cfg->tps_warn = new_cfg.tps_warn;
        cfg->tps_error = new_cfg.tps_error;
        cfg->monitor_interval = new_cfg.monitor_interval;
        cfg->log_level = new_cfg.log_level;

        if (g_logger) {
            g_logger->set_level(parse_log_level(new_cfg.log_level));
        }
    }
};

static atomic<bool> g_child_quit{false};

void child_signal_handler(int) {
    g_child_quit = true;
}

int run_server(const string& config_path) {
    Config cfg;
    load_config_file(config_path, cfg);

    AsyncLogger logger(cfg.log_file, parse_log_level(cfg.log_level), cfg.log_batch, cfg.log_flush);
    g_logger = &logger;

    signal(SIGINT, child_signal_handler);
    signal(SIGTERM, child_signal_handler);
    signal(SIGPIPE, SIG_IGN);

    ThreadPool pool(cfg.thread_num);
    EpollServer server(cfg.server_port, pool);

    thread server_thread([&]() { server.run(); });

    atomic<bool> monitor_running{true};
    thread monitor_thread(monitor, ref(server), ref(monitor_running), ref(cfg));

    ConfigManager cfg_mgr(config_path, &cfg, &pool);
    thread config_thread([&]() { cfg_mgr.watch_loop(); });

    while (!g_child_quit) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    glog(LogLevel::INFO, "Shutting down...");

    cfg_mgr.stop();
    if (config_thread.joinable()) {
        config_thread.join();
    }

    monitor_running = false;
    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    pool.shutdown();

    cout << "[Server] Total requests: " << server.get_total_tps() << endl;
    glog(LogLevel::INFO, "[Server] Total requests: " + to_string(server.get_total_tps()));
    glog(LogLevel::INFO, "Server exited gracefully.");

    g_logger = nullptr;
    logger.shutdown();
    return 0;
}

static pid_t g_child_pid = 0;

void watchdog_signal_handler(int sig) {
    if (g_child_pid > 0) {
        kill(g_child_pid, sig);
    }
}

int main(int argc, char* argv[]) {
    string config_path = "config.json";
    bool use_watchdog = true;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--no-watchdog") {
            use_watchdog = false;
        } else {
            config_path = arg;
        }
    }

    if (!use_watchdog) {
        signal(SIGINT, child_signal_handler);
        signal(SIGTERM, child_signal_handler);
        return run_server(config_path);
    }

    signal(SIGINT, watchdog_signal_handler);
    signal(SIGTERM, watchdog_signal_handler);

    while (true) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            return 1;
        }

        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            g_child_quit = false;
            g_logger = nullptr;
            exit(run_server(config_path));
        }

        g_child_pid = pid;
        cout << "[Watchdog] started child pid=" << pid << endl;

        int status = 0;
        waitpid(pid, &status, 0);
        g_child_pid = 0;

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            cout << "[Watchdog] child exited with code " << code << endl;
            if (code == 0) {
                cout << "[Watchdog] normal shutdown." << endl;
                break;
            }
        }

        if (WIFSIGNALED(status)) {
            cout << "[Watchdog] child killed by signal " << WTERMSIG(status) << endl;
        }

        cout << "[Watchdog] restarting in 2 seconds..." << endl;
        sleep(2);
    }

    return 0;
}
