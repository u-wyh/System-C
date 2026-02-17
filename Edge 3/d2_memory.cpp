#include <iostream>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

class File {
private:
    unique_ptr<FILE, decltype(&fclose)> file;

public:
    File(const string& name, const string& mode)
        : file(fopen(name.c_str(), mode.c_str()), fclose) {
        if (!file)
            throw runtime_error("Failed to open file: " + name);
    }

    void write(const string& data) {
        fwrite(data.c_str(), 1, data.size(), file.get());
        fflush(file.get());
    }
};

class Socket {
private:
    int fd;

public:
    explicit Socket(int port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw runtime_error("socket failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw runtime_error("bind failed");

        if (listen(fd, 10) < 0)
            throw runtime_error("listen failed");
    }

    int accept_client() {
        int client_fd = accept(fd, nullptr, nullptr);
        if (client_fd < 0)
            throw runtime_error("accept failed");
        return client_fd;
    }

    ~Socket() {
        if (fd >= 0)
            close(fd);
    }
};

class ClientSocket {
private:
    int fd;

public:
    explicit ClientSocket(int client_fd) : fd(client_fd) {}

    ssize_t recv_data(string& msg) {
        char buffer[1024];
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            buffer[n] = 0;
            msg = buffer;
        }
        return n;
    }

    void send_data(const string& msg) {
        send(fd, msg.c_str(), msg.size(), 0);
    }

    ~ClientSocket() {
        if (fd >= 0)
            close(fd);
    }
};

class ThreadWrapper {
private:
    thread t;

public:
    template<typename Func,typename... Args>
    explicit ThreadWrapper(Func&& f,Args&&... args){
        try{
            t=thread(forward<Func>(f),forward<Args>(args)...);
        }
        catch(const system_error&e){
            cerr<<"Thread creation failed: "<<e.what()<<endl;
            throw;
        }
    }

    ~ThreadWrapper() {
        if (t.joinable())
            t.join();
    }

    ThreadWrapper(const ThreadWrapper&) = delete;
    ThreadWrapper& operator=(const ThreadWrapper&) = delete;

    ThreadWrapper(ThreadWrapper&&) = default;
};

class Service {
private:
    File logFile{"service.log", "w"};
    Socket server{8080};
    vector<ThreadWrapper> workers;
    atomic<bool> running{false};

public:
    void log_worker() {
        while (running) {
            logFile.write("log entry\n");
            this_thread::sleep_for(chrono::seconds(1));
        }
    }

    void tcp_worker() {
        while (running) {
            try {
                int fd = server.accept_client();

                // 用 RAII 保证异常安全
                ClientSocket client(fd);

                string msg;
                while (client.recv_data(msg) > 0) {
                    client.send_data(msg);
                }
            }
            catch (...) {
                if (running)
                    cerr << "worker error\n";
            }
        }
    }

    void start() {
        running = true;

        workers.emplace_back(&Service::log_worker,this);
        workers.emplace_back(&Service::tcp_worker,this);
    }

    void stop() {
        running = false;
        workers.clear();  // 自动 join
    }
};

int main() {
    try {
        Service service;
        service.start();

        this_thread::sleep_for(chrono::seconds(10));

        service.stop();
    }
    catch (const exception& e) {
        cerr << "[Fatal] " << e.what() << endl;
    }

    return 0;
}
