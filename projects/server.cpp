#include <arpa/inet.h>
#include <fcntl.h>
#include <iconv.h>
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
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

using json = nlohmann::json;
using namespace std;

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

    // AI接口配置，从config.json读取
    string ai_api_key = "";
    string ai_model = "qwen-turbo";
    string ai_base_url = "dashscope.aliyuncs.com";
    string ai_path = "/compatible-mode/v1/chat/completions";
    int ai_timeout_s = 30;
    int ai_max_concurrent = 8;
    int ai_max_history = 10;
    string system_prompt = "You are a helpful assistant. Reply concisely.";
};

enum class LogLevel { 
    DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 
};

class AsyncLogger {
private:
    vector<string> buffer1, buffer2;
    atomic<bool> swap_flag{false};
    mutex mtx;
    condition_variable cv;
    atomic<bool> running{true};
    thread worker;
    ofstream fout;
    string filename;
    atomic<LogLevel> level;
    size_t batch;
    int flush_ms;

public:
    AsyncLogger(const string& fn, LogLevel lvl = LogLevel::INFO,size_t batch_size = 256, int flush_interval = 50)
        : batch(batch_size), flush_ms(flush_interval), level(lvl) 
    {
        filename = fn;
        fout.open(fn, ios::app);
        buffer1.reserve(batch * 2);
        buffer2.reserve(batch * 2);
        worker = thread(&AsyncLogger::process, this);
    }
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    ~AsyncLogger() { shutdown(); }

    string now_time() {
        auto now = chrono::system_clock::now();
        time_t t = chrono::system_clock::to_time_t(now);
        tm tm_buf{};
        localtime_r(&t, &tm_buf);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return string(buf);
    }

    // 双缓冲：后台线程负责flush，业务线程只写缓冲区
    void process() {
        vector<string>*current, *back;
        int log_line_count = 0;
        const int max_lines = 1000;
        while (running || !buffer1.empty() || !buffer2.empty()) {
            {
                unique_lock<mutex> lock(mtx);
                cv.wait_for(lock, chrono::milliseconds(flush_ms), [this]() {
                    return !buffer1.empty() || !buffer2.empty() || !running;
                });
                swap_flag = !swap_flag;
                if (swap_flag) {
                    current = &buffer1;
                    back = &buffer2;
                } else {
                    current = &buffer2;
                    back = &buffer1;
                }
                current->swap(*back);
            }
            for (auto& line : *current) {
                cout << line << "\n";
                if (fout.is_open()) {
                    fout << line << "\n";
                    if (++log_line_count >= max_lines) {
                        fout.close();
                        fout.open(filename, ios::trunc);
                        log_line_count = 0;
                    }
                }
            }
            if (fout.is_open()) {
                fout.flush();
            }
            current->clear();
        }
    }

    void shutdown() {
        if (!running) return;
        running = false;
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        if (fout.is_open()) {
            fout.close();
        }
    }

    void log(LogLevel lvl, const string& msg) {
        if (lvl < level.load() || !running) return;
        const char* s = "";
        switch (lvl) {
            case LogLevel::DEBUG:
                s = "DEBUG";
                break;
            case LogLevel::INFO:
                s = "INFO";
                break;
            case LogLevel::WARN:
                s = "WARN";
                break;
            case LogLevel::ERROR:
                s = "ERROR";
                break;
        }
        string line = "[" + now_time() + "][" + s + "] " + msg;
        {
            lock_guard<mutex> lock(mtx);
            (swap_flag ? buffer1 : buffer2).push_back(move(line));
        }
        cv.notify_one();
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
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running{true};
    atomic<size_t> target_thread_num;

public:
    explicit ThreadPool(size_t n) : target_thread_num(n) { resize(n); }
    ~ThreadPool() { shutdown(); }

    void submit(function<void()> task) {
        lock_guard<mutex> lock(queueMutex);
        if (!running) return;
        tasks.push(move(task));
        condvar.notify_one();
    }

    void resize(size_t newSize) {
        target_thread_num = newSize;
        lock_guard<mutex> lock(queueMutex);
        for (size_t i = workers.size(); i < newSize; ++i){
            workers.emplace_back([this]() { worker_loop(); });
        }
        glog(LogLevel::INFO, "[ThreadPool] resized to " + to_string(newSize));
    }

    void shutdown() {
        {
            lock_guard<mutex> lock(queueMutex);
            if (!running) return;
            running = false;
        }
        condvar.notify_all();
        for (auto& t : workers){
            if (t.joinable()) {
                t.join();
            }
        }
        workers.clear();
    }

    void worker_loop() {
        while (true) {
            function<void()> task;
            {
                unique_lock<mutex> lock(queueMutex);
                condvar.wait(lock, [this]() { 
                    return !tasks.empty() || !running; 
                });
                if (!running && tasks.empty()) return;
                if (workers.size() > target_thread_num) return;
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

// 信号量，用于限制并发AI请求数
class AiSemaphore {
private:
    mutex mtx;
    condition_variable cv;
    int count;
    int max_count;

public:
    explicit AiSemaphore(int n) : count(n), max_count(n) {}

    bool try_acquire() {
        lock_guard<mutex> lock(mtx);
        if (count <= 0) return false;
        count--;
        return true;
    }

    void release() {
        lock_guard<mutex> lock(mtx);
        count++;
        cv.notify_one();
    }

    int available() {
        lock_guard<mutex> lock(mtx);
        return count;
    }

    void set_max(int n) {
        lock_guard<mutex> lock(mtx);
        count += (n - max_count);
        max_count = n;
        cv.notify_all();
    }
};

static AiSemaphore* g_ai_sem = nullptr;

class AiClient {
public:
    // 纯ASCII直接返回；否则尝试GBK->UTF-8转换（兼容telnet客户端）
    static string to_utf8(const string& s) {
        bool is_ascii = true;
        for (unsigned char c : s) {
            if (c >= 0x80) {
                is_ascii = false;
                break;
            }
        }
        if (is_ascii) return s;

        iconv_t cd = iconv_open("UTF-8", "GBK");
        if (cd != (iconv_t)-1) {
            string out;
            out.resize(s.size() * 3);
            char* in_buf = const_cast<char*>(s.c_str());
            char* out_buf = &out[0];
            size_t in_left = s.size();
            size_t out_left = out.size();
            size_t ret = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
            iconv_close(cd);
            if (ret != (size_t)-1) {
                out.resize(out.size() - out_left);
                return out;
            }
        }

        if (is_valid_utf8(s)) return s;
        return sanitize_utf8(s);
    }

    static bool is_valid_utf8(const string& s) {
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = s[i];
            int bytes = 0;
            if (c < 0x80)
                bytes = 1;
            else if ((c & 0xE0) == 0xC0)
                bytes = 2;
            else if ((c & 0xF0) == 0xE0)
                bytes = 3;
            else if ((c & 0xF8) == 0xF0)
                bytes = 4;
            else
                return false;
            if (i + bytes > s.size()) return false;
            for (int b = 1; b < bytes; b++)
                if ((s[i + b] & 0xC0) != 0x80) return false;
            i += bytes;
        }
        return true;
    }

    // 过滤非法字节，兜底用
    static string sanitize_utf8(const string& s) {
        string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = s[i];
            int bytes = 0;
            if (c < 0x80)
                bytes = 1;
            else if ((c & 0xE0) == 0xC0)
                bytes = 2;
            else if ((c & 0xF0) == 0xE0)
                bytes = 3;
            else if ((c & 0xF8) == 0xF0)
                bytes = 4;
            else {
                i++;
                continue;
            }
            if (i + bytes > s.size()) break;
            bool valid = true;
            for (int b = 1; b < bytes; b++)
                if ((s[i + b] & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
            if (valid) out.append(s, i, bytes);
            i += bytes;
        }
        return out;
    }

    static string ask(const string& user_msg, const Config& cfg,vector<json>& history) {
        if (g_ai_sem && !g_ai_sem->try_acquire()) {
            glog(LogLevel::WARN, "[AiClient] rate limit reached");
            return "服务繁忙，请稍后再试。";
        }

        struct SemGuard {
            ~SemGuard() {
                if (g_ai_sem) g_ai_sem->release();
            }
        } sem_guard;

        string clean_msg = to_utf8(user_msg);
        if (clean_msg.empty()) return "消息包含无法识别的字符，请重新发送。";

        // 构造messages数组，带上历史上下文
        json messages = json::array();
        messages.push_back( {{"role", "system"}, {"content", cfg.system_prompt}});
        for (auto& h : history) messages.push_back(h);
        messages.push_back({{"role", "user"}, {"content", clean_msg}});

        json body = {{"model", cfg.ai_model}, {"messages", messages}};

        httplib::SSLClient cli(cfg.ai_base_url);
        cli.set_connection_timeout(cfg.ai_timeout_s);
        cli.set_read_timeout(cfg.ai_timeout_s);
        cli.enable_server_certificate_verification(false);

        httplib::Headers headers = {
            {"Authorization", "Bearer " + cfg.ai_api_key},
            {"Content-Type", "application/json"}};

        auto res = cli.Post(cfg.ai_path, headers, body.dump(), "application/json");

        if (!res) {
            string err = httplib::to_string(res.error());
            glog(LogLevel::ERROR, "[AiClient] HTTP error: " + err);
            return "[Error] Failed to reach AI API: " + err;
        }
        if (res->status != 200) {
            glog(LogLevel::ERROR, "[AiClient] HTTP " + to_string(res->status) + " " + res->body);
            return "[Error] AI API returned HTTP " + to_string(res->status);
        }

        try {
            auto j = json::parse(res->body);
            return j["choices"][0]["message"]["content"].get<string>();
        } catch (const exception& e) {
            glog(LogLevel::ERROR, "[AiClient] parse error: " + string(e.what()) + " body=" + res->body);
            return "[Error] Failed to parse AI response";
        }
    }
};

struct ClientContext {
    string recv_buf;
    bool busy = false;
    vector<json> history;  // 多轮对话历史
};

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class EpollServer {
private:
    int listen_fd = -1;
    int epfd = -1;
    ThreadPool& pool;
    Config& cfg;
    atomic<bool> running{true};

    mutex clients_mtx;
    unordered_map<int, ClientContext> clients;

    atomic<int> tps{0};
    atomic<int64_t> total_tps{0};

public:
    EpollServer(int port, ThreadPool& tp, Config& c) : pool(tp), cfg(c) {
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

        glog(LogLevel::INFO, "[Server] listening on port " + to_string(port));
    }

    void run() {
        vector<epoll_event> events(1024);
        while (running) {
            int n = epoll_wait(epfd, events.data(), events.size(), 500);
            if (n < 0) {
                if (errno == EINTR) {
                    if (!running)
                        break;
                    else
                        continue;
                }
                if (running) perror("epoll_wait");
                break;
            }
            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                if (fd == listen_fd)
                    accept_client();
                else
                    handle_client(fd);
            }
        }
        {
            lock_guard<mutex> lock(clients_mtx);
            for (auto& kv : clients) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, kv.first, nullptr);
                close(kv.first);
            }
            clients.clear();
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

    void stop() { running = false; }

    void accept_client() {
        while (true) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = accept(listen_fd, (sockaddr*)&client, &len);
            if (cfd < 0) {
                if (errno == EAGAIN) break;
                continue;
            }
            set_nonblocking(cfd);
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = cfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
            {
                lock_guard<mutex> lock(clients_mtx);
                clients.emplace(cfd, ClientContext{});
            }
            glog(LogLevel::DEBUG, "[Server] new client fd=" + to_string(cfd));
        }
    }

    void close_client(int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        lock_guard<mutex> lock(clients_mtx);
        clients.erase(fd);
    }

    void handle_client(int fd) {
        char buf[4096];
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                {
                    lock_guard<mutex> lock(clients_mtx);
                    clients[fd].recv_buf.append(buf, n);
                }
                process_buffer(fd);
            } else if (n == 0) {
                close_client(fd);
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_client(fd);
                break;
            }
        }
    }

    void process_buffer(int fd) {
        while (true) {
            string msg;
            {
                lock_guard<mutex> lock(clients_mtx);
                auto it = clients.find(fd);
                if (it == clients.end()) break;
                auto& ctx = it->second;

                // 上一条消息还在等AI回复，缓存新输入稍后处理
                if (ctx.busy) break;

                size_t pos = ctx.recv_buf.find('\n');
                if (pos == string::npos) break;

                msg = ctx.recv_buf.substr(0, pos);
                if (!msg.empty() && msg.back() == '\r') msg.pop_back();
                ctx.recv_buf.erase(0, pos + 1);

                if (msg.empty()) continue;
                ctx.busy = true;
            }

            tps++;
            total_tps++;

            pool.submit([this, fd, msg]() {
                // RAII保证busy一定被重置，防止连接卡死
                auto reset_busy = [&]() {
                    lock_guard<mutex> lock(clients_mtx);
                    auto it = clients.find(fd);
                    if (it != clients.end()) it->second.busy = false;
                };

                glog(LogLevel::INFO,
                     "[AI] fd=" + to_string(fd) + " question=" + msg);

                // 持锁拷贝history，避免长时间持锁等待AI
                vector<json> history_snapshot;
                {
                    lock_guard<mutex> lock(clients_mtx);
                    auto it = clients.find(fd);
                    if (it != clients.end())
                        history_snapshot = it->second.history;
                }

                string reply;
                try {
                    reply = AiClient::ask(msg, cfg, history_snapshot);
                } catch (...) {
                    reply = "[Error] Internal error";
                }

                glog(LogLevel::INFO,
                     "[AI] fd=" + to_string(fd) + " answer=" + reply);

                // 更新对话历史并写回
                if (reply.substr(0, 7) != "[Error]" &&
                    reply != "服务繁忙，请稍后再试。") {
                    history_snapshot.push_back({{"role", "user"}, {"content", msg}});
                    history_snapshot.push_back({{"role", "assistant"}, {"content", reply}});

                    int max_turns = cfg.ai_max_history * 2;
                    while ((int)history_snapshot.size() > max_turns)
                        history_snapshot.erase(history_snapshot.begin(),history_snapshot.begin() + 2);

                    lock_guard<mutex> lock(clients_mtx);
                    auto it = clients.find(fd);
                    if (it != clients.end())
                        it->second.history = move(history_snapshot);
                }

                string out = reply + "\n";

                {
                    lock_guard<mutex> lock(clients_mtx);
                    if (clients.find(fd) == clients.end()) return;
                }

                ssize_t total = static_cast<ssize_t>(out.size());
                ssize_t sent = 0;
                while (sent < total) {
                    ssize_t n = write(fd, out.c_str() + sent, total - sent);
                    if (n <= 0) break;
                    sent += n;
                }

                reset_busy();
                process_buffer(fd);
            });
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
    uint64_t lastTotalTime = 0;
    auto lastCheck = chrono::steady_clock::now();
    while (running) {
        this_thread::sleep_for(chrono::milliseconds(cfg.monitor_interval));
        if (!running) break;

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

        ifstream fstatus("/proc/self/status");
        string line;
        size_t memKB = 0;
        while (getline(fstatus, line))
            if (line.find("VmRSS:") != string::npos) {
                stringstream ss(line);
                string key;
                ss >> key >> memKB;
            }

        int cur_tps = server.get_tps();
        {
            stringstream ss;
            ss << "[Monitor] CPU=" << cpuUsage << "% Mem=" << memKB / 1024.0 << "MB TPS=" << cur_tps;
            glog(LogLevel::INFO, ss.str());
        }

        if (cpuUsage >= cfg.cpu_error)
            glog(LogLevel::ERROR, "[ALARM] CPU too high: " + to_string(cpuUsage) + "%");
        else if (cpuUsage >= cfg.cpu_warn)
            glog(LogLevel::WARN, "[ALARM] CPU warning: " + to_string(cpuUsage) + "%");

        double memMB = memKB / 1024.0;
        if (memMB >= cfg.mem_error)
            glog(LogLevel::ERROR, "[ALARM] Mem too high: " + to_string(memMB) + "MB");
        else if (memMB >= cfg.mem_warn)
            glog(LogLevel::WARN, "[ALARM] Mem warning: " + to_string(memMB) + "MB");

        if (cur_tps >= cfg.tps_error)
            glog(LogLevel::ERROR, "[ALARM] TPS too high: " + to_string(cur_tps));
        else if (cur_tps >= cfg.tps_warn)
            glog(LogLevel::WARN, "[ALARM] TPS warning: " + to_string(cur_tps));
    }
}

class ConfigManager {
private:
    string filename;
    Config* cfg;
    atomic<bool> running{true};
    ThreadPool* pool;
    time_t last_mtime{0};

public:
    ConfigManager(const string& file, Config* c, ThreadPool* p)
        : filename(file), cfg(c), pool(p) {}

    void stop() { running = false; }

    void watch_loop() {
        while (running) {
            this_thread::sleep_for(chrono::milliseconds(cfg->monitor_interval));
            if (!running) break;
            struct stat st;
            if (stat(filename.c_str(), &st) == 0 && st.st_mtime != last_mtime) {
                last_mtime = st.st_mtime;
                update_config();
            }
        }
    }

    void update_config() {
        ifstream f(filename);
        if (!f.is_open()) return;
        json j;
        try {
            f >> j;
        } catch (...) {
            glog(LogLevel::WARN, "[ConfigManager] failed to parse config");
            return;
        }

        if (j.contains("thread_num")) {
            int n = j["thread_num"];
            if (n != cfg->thread_num) {
                glog(LogLevel::INFO, "[ConfigManager] thread_num: " + to_string(cfg->thread_num) + " -> " + to_string(n));
                pool->resize(n);
                cfg->thread_num = n;
            }
        }
        if (j.contains("log_level") && g_logger) {
            string lvl = j["log_level"];
            LogLevel newLvl = LogLevel::INFO;
            if (lvl == "DEBUG")
                newLvl = LogLevel::DEBUG;
            else if (lvl == "WARN")
                newLvl = LogLevel::WARN;
            else if (lvl == "ERROR")
                newLvl = LogLevel::ERROR;
            g_logger->set_level(newLvl);
        }

        if (j.contains("ai_api_key")) cfg->ai_api_key = j["ai_api_key"];
        if (j.contains("ai_model")) cfg->ai_model = j["ai_model"];
        if (j.contains("ai_timeout_s")) cfg->ai_timeout_s = j["ai_timeout_s"];
        if (j.contains("system_prompt")) cfg->system_prompt = j["system_prompt"];
        if (j.contains("ai_max_history")) cfg->ai_max_history = j["ai_max_history"];
        if (j.contains("ai_max_concurrent") && g_ai_sem) {
            int n = j["ai_max_concurrent"];
            if (n != cfg->ai_max_concurrent) {
                glog(LogLevel::INFO, "[ConfigManager] ai_max_concurrent: " + to_string(cfg->ai_max_concurrent) + " -> " + to_string(n));
                g_ai_sem->set_max(n);
                cfg->ai_max_concurrent = n;
            }
        }
    }
};

static atomic<bool> g_child_quit{false};
void child_signal_handler(int) { g_child_quit = true; }

int run_server() {
    AsyncLogger logger("server.log", LogLevel::INFO);
    g_logger = &logger;

    signal(SIGINT, child_signal_handler);
    signal(SIGTERM, child_signal_handler);

    Config cfg;

    // 启动时读一次配置，拿到api_key
    {
        ifstream f("config.json");
        if (f.is_open()) {
            json j;
            try {
                f >> j;
                if (j.contains("ai_api_key")) cfg.ai_api_key = j["ai_api_key"];
                if (j.contains("ai_model")) cfg.ai_model = j["ai_model"];
                if (j.contains("system_prompt")) cfg.system_prompt = j["system_prompt"];
                if (j.contains("thread_num")) cfg.thread_num = j["thread_num"];
            } catch (...) {
                glog(LogLevel::WARN, "[Startup] failed to parse config.json");
            }
        }
    }

    if (cfg.ai_api_key.empty())
        glog(LogLevel::WARN,
             "[Startup] ai_api_key is empty! Set it in config.json");

    AiSemaphore ai_sem(cfg.ai_max_concurrent);
    g_ai_sem = &ai_sem;

    ThreadPool pool(cfg.thread_num);
    EpollServer server(cfg.server_port, pool, cfg);

    thread server_thread([&]() { server.run(); });

    atomic<bool> monitor_running{true};
    thread mon_thread(monitor, ref(server), ref(monitor_running), ref(cfg));

    ConfigManager cfgMgr("config.json", &cfg, &pool);
    thread cfg_thread([&]() { cfgMgr.watch_loop(); });

    while (!g_child_quit) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    glog(LogLevel::INFO, "Shutting down...");

    cfgMgr.stop();
    if (cfg_thread.joinable()) {
        cfg_thread.join();
    }

    monitor_running = false;
    if (mon_thread.joinable()) {
        mon_thread.join();
    }

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    pool.shutdown();
    g_ai_sem = nullptr;

    cout << "[Server] Total requests: " << server.get_total_tps() << endl;
    glog(LogLevel::INFO, "[Server] Total requests: " + to_string(server.get_total_tps()));
    glog(LogLevel::INFO, "Server exited gracefully.");

    g_logger = nullptr;
    logger.shutdown();
    return 0;
}

static pid_t g_child_pid = 0;
void watchdog_signal_handler(int sig) {
    if (g_child_pid > 0) kill(g_child_pid, sig);
}

int main() {
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
            exit(run_server());
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
        if (WIFSIGNALED(status))
            cout << "[Watchdog] child killed by signal " << WTERMSIG(status)
                 << endl;

        cout << "[Watchdog] restarting in 2 seconds..." << endl;
        sleep(2);
    }
    return 0;
}
// g++ -O2 -std=c++17 server.cpp -o ai_server \
//     -lssl -lcrypto -lpthread \
//     -DCPPHTTPLIB_OPENSSL_SUPPORT