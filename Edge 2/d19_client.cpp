#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <atomic>

using namespace std;

atomic<long> total_send{0};
atomic<long> total_error{0};
atomic<bool> running{true};

void client_worker(const string &server_ip, int server_port, int id)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket failed");
        total_error++;
        return;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton failed");
        close(sockfd);
        total_error++;
        return;
    }

    if (connect(sockfd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect error");
        close(sockfd);
        total_error++;
        return;
    }

    cout << "[client " << id << "] connected." << endl;

    const char *msg = "hello world\n";

    while (running)
    {
        ssize_t n = send(sockfd, msg, strlen(msg), 0);
        if (n > 0)
        {
            total_send++;
        }
        else
        {
            total_error++;
            break;
        }

        this_thread::sleep_for(chrono::milliseconds(10)); 
        // 你可以改成 1ms 测极限
        // 或 0 测满速
    }

    close(sockfd);
}

int main()
{
    int num_connections = 500;     // 并发连接数
    int duration_seconds = 60;     // 压测时间
    string server_ip = "127.0.0.1";
    int server_port = 8080;

    vector<thread> threads;
    threads.reserve(num_connections);

    cout << "Starting stress test..." << endl;
    cout << "Connections: " << num_connections << endl;
    cout << "Duration: " << duration_seconds << " seconds" << endl;

    for (int i = 0; i < num_connections; i++)
    {
        threads.emplace_back(client_worker, server_ip, server_port, i);
        this_thread::sleep_for(chrono::milliseconds(5));
    }

    // 监控线程
    thread monitor([]()
                   {
                       long last = 0;
                       while (running)
                       {
                           this_thread::sleep_for(chrono::seconds(1));
                           long now = total_send.load();
                           cout << "QPS = " << (now - last)
                                << " | Total = " << now
                                << " | Errors = " << total_error.load()
                                << endl;
                           last = now;
                       } });

    this_thread::sleep_for(chrono::seconds(duration_seconds));
    running = false;

    for (auto &t : threads)
    {
        if (t.joinable())
            t.join();
    }

    if (monitor.joinable())
        monitor.join();

    cout << "Test finished." << endl;
    cout << "Final total send: " << total_send.load() << endl;
    cout << "Final total error: " << total_error.load() << endl;

    return 0;
}