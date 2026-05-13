#include <arpa/inet.h>
#include <csignal>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>

using namespace std;

struct Options {
    string host = "188.166.186.248";
    int port = 8080;
    int clients = 500;
    int messages_per_client = 2000;
    int interval_ms = 2;
};

atomic<long long> g_sent{0};
atomic<long long> g_received{0};
atomic<long long> g_connect_ok{0};
atomic<long long> g_connect_fail{0};
atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

void print_usage(const char* prog) {
    cout << "Usage: " << prog
         << " [host] [port] [clients] [messages_per_client] [interval_ms]\n"
         << "Defaults:\n"
         << "  host=127.0.0.1\n"
         << "  port=8080\n"
         << "  clients=500\n"
         << "  messages_per_client=2000\n"
         << "  interval_ms=2\n";
}

bool parse_int(const char* text, int& value) {
    char* end = nullptr;
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool recv_line(int sock, string& line) {
    line.clear();
    char ch = 0;
    while (g_running) {
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n > 0) {
            line.push_back(ch);
            if (ch == '\n') {
                return true;
            }
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return false;
}

void client_worker(const Options& opt, int id) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        g_connect_fail++;
        return;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(opt.port);
    if (inet_pton(AF_INET, opt.host.c_str(), &server_addr.sin_addr) <= 0) {
        cerr << "[Worker " << id << "] invalid host: " << opt.host << endl;
        close(sock);
        g_connect_fail++;
        return;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        g_connect_fail++;
        return;
    }
    g_connect_ok++;

    for (int i = 0; i < opt.messages_per_client && g_running; ++i) {
        if (opt.interval_ms > 0) {
            this_thread::sleep_for(chrono::milliseconds(opt.interval_ms));
        }

        string msg = "msg " + to_string(id) + ":" + to_string(i) + "\n";
        ssize_t sent = send(sock, msg.c_str(), msg.size(), 0);
        if (sent != static_cast<ssize_t>(msg.size())) {
            break;
        }
        g_sent++;

        string reply;
        if (!recv_line(sock, reply)) {
            break;
        }
        g_received++;
    }

    close(sock);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    Options opt;
    if (argc > 1 && string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }
    if (argc > 1) opt.host = argv[1];
    if (argc > 2 && !parse_int(argv[2], opt.port)) {
        cerr << "Invalid port: " << argv[2] << endl;
        return 1;
    }
    if (argc > 3 && !parse_int(argv[3], opt.clients)) {
        cerr << "Invalid clients: " << argv[3] << endl;
        return 1;
    }
    if (argc > 4 && !parse_int(argv[4], opt.messages_per_client)) {
        cerr << "Invalid messages_per_client: " << argv[4] << endl;
        return 1;
    }
    if (argc > 5 && !parse_int(argv[5], opt.interval_ms)) {
        cerr << "Invalid interval_ms: " << argv[5] << endl;
        return 1;
    }

    cout << "[Client] host=" << opt.host
         << " port=" << opt.port
         << " clients=" << opt.clients
         << " messages_per_client=" << opt.messages_per_client
         << " interval_ms=" << opt.interval_ms << endl;

    auto start = chrono::steady_clock::now();
    vector<thread> workers;
    workers.reserve(opt.clients);

    for (int i = 1; i <= opt.clients; ++i) {
        workers.emplace_back(client_worker, cref(opt), i);
    }

    thread monitor([&]() {
        long long last_recv = 0;
        int sec = 0;
        while (g_running) {
            this_thread::sleep_for(chrono::seconds(1));
            long long cur_sent = g_sent.load();
            long long cur_recv = g_received.load();
            long long qps = cur_recv - last_recv;
            cout << "[Client Monitor] sec=" << ++sec
                 << " sent=" << cur_sent
                 << " received=" << cur_recv
                 << " qps=" << qps
                 << " connect_ok=" << g_connect_ok.load()
                 << " connect_fail=" << g_connect_fail.load()
                 << endl;
            last_recv = cur_recv;

            long long target = static_cast<long long>(opt.clients) * opt.messages_per_client;
            if (cur_recv >= target) {
                break;
            }
        }
    });

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    g_running = false;
    if (monitor.joinable()) {
        monitor.join();
    }

    auto end = chrono::steady_clock::now();
    double elapsed = chrono::duration<double>(end - start).count();
    long long total_sent = g_sent.load();
    long long total_recv = g_received.load();
    double avg_qps = elapsed > 0.0 ? total_recv / elapsed : 0.0;

    cout << "[Summary] elapsed=" << elapsed << "s"
         << " sent=" << total_sent
         << " received=" << total_recv
         << " avg_qps=" << avg_qps
         << " connect_ok=" << g_connect_ok.load()
         << " connect_fail=" << g_connect_fail.load()
         << endl;

    return 0;
}
