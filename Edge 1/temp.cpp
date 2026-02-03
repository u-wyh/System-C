#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <mutex>
#include <stdexcept>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

////////////////////////////////////////////////////////////
// File 资源（你写的）
////////////////////////////////////////////////////////////
class File{
private:
    FILE* fptr;

    void close(){
        if(fptr){
            cout<<"[File] closed"<<endl;
            fclose(fptr);
            fptr=nullptr;
        }
    }

public:
    File(const string& filename,const string& mode):fptr(nullptr){
        fptr=fopen(filename.c_str(),mode.c_str());
        if(!fptr){
            cerr<<"Failed to open file : "<<filename<<endl;
        }
    }

    File(const File&)=delete;
    File& operator=(const File&)=delete;

    File(File&& other) noexcept:fptr(other.fptr){
        cout<<"[File] Move Constructor"<<endl;
        other.fptr=nullptr;
    }

    File& operator=(File&& other) noexcept{
        cout<<"[File] Move Assignment"<<endl;
        if(this!=&other){
            close();
            fptr=other.fptr;
            other.fptr=nullptr;
        }
        return *this;
    }

    ~File(){
        close();
    }

    bool is_valid()const{
        return fptr!=nullptr;
    }

    bool write(const string& data){
        if(!fptr) return false;
        return fwrite(data.c_str(),1,data.size(),fptr)==data.size();
    }

    string read_line(){
        if(!fptr) return "";
        char buffer[1024];
        if(fgets(buffer,sizeof(buffer),fptr)){
            return string(buffer);
        }
        return "";
    }

    void seek_to_begin(){
        if(fptr) fseek(fptr,0,SEEK_SET);
    }

    void flush(){
        if(fptr) fflush(fptr);
    }
};

////////////////////////////////////////////////////////////
// Mutex + LockGuard
////////////////////////////////////////////////////////////
class Mutex{
private:
    mutex a_mutex;
    friend class LockGuard;

public:
    Mutex()=default;
    ~Mutex()=default;

    Mutex(const Mutex&)=delete;
    Mutex& operator=(const Mutex&)=delete;

    Mutex(Mutex&&)=default;
    Mutex& operator=(Mutex&&)=default;

    void lock(){ a_mutex.lock(); }
    void unlock(){ a_mutex.unlock(); }
    bool try_lock(){ return a_mutex.try_lock(); }
};

class LockGuard{
private:
    Mutex& a_mutex;

public:
    LockGuard(Mutex& mutex):a_mutex(mutex){ a_mutex.lock(); }
    ~LockGuard(){ a_mutex.unlock(); }

    LockGuard(const LockGuard&)=delete;
    LockGuard& operator=(const LockGuard&)=delete;
};

////////////////////////////////////////////////////////////
// Socket
////////////////////////////////////////////////////////////
enum class Protocol{ TCP, UDP };

class Socket{
private:
    int fd;
    int port;
    Protocol protocol;

    void close(){
        if(fd!=-1){
            ::close(fd);
            cout<<"[Socket] closed "<<get_protocol()<<" socket on port "<<port<<endl;
            fd=-1;
        }
    }

public:
    Socket(int port_, Protocol protocol_):fd(-1),port(port_),protocol(protocol_){
        int type=(protocol==Protocol::TCP)?SOCK_STREAM:SOCK_DGRAM;
        fd=::socket(AF_INET,type,0);
        if(fd==-1){
            throw runtime_error("socket() failed: "+string(strerror(errno)));
        }

        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=INADDR_ANY;
        addr.sin_port=htons(port);

        if(::bind(fd,(sockaddr*)&addr,sizeof(addr))<0){
            ::close(fd);
            fd=-1;
            throw runtime_error("bind() failed on port "+to_string(port)+" : "+string(strerror(errno)));
        }

        cout<<"[Socket] created "<<get_protocol()<<" socket on port "<<port<<endl;
    }

    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;

    Socket(Socket&& other) noexcept: fd(other.fd), port(other.port), protocol(other.protocol){
        other.fd=-1;
    }
    Socket& operator=(Socket&& other) noexcept{
        if(this!=&other){
            close();
            fd=other.fd;
            port=other.port;
            protocol=other.protocol;
            other.fd=-1;
        }
        return *this;
    }

    ~Socket(){ close(); }

    string get_protocol(){ return (protocol==Protocol::TCP)?"TCP":"UDP"; }
};

////////////////////////////////////////////////////////////
// Thread
////////////////////////////////////////////////////////////
class Thread{
private:
    thread t;

public:
    template<typename Func,typename... Args>
    explicit Thread(Func&& f, Args&&... args){
        t=thread(forward<Func>(f),forward<Args>(args)...);
    }

    ~Thread(){
        if(t.joinable()) t.join();
    }

    Thread(const Thread&)=delete;
    Thread& operator=(const Thread&)=delete;

    Thread(Thread&& other) noexcept: t(move(other.t)){}
    Thread& operator=(Thread&& other) noexcept{
        if(this!=&other){
            if(t.joinable()) t.join();
            t=move(other.t);
        }
        return *this;
    }
};

////////////////////////////////////////////////////////////
// Service
////////////////////////////////////////////////////////////
class Service{
private:
    File logFile{"service.log","w"};
    Socket serverSocket{8080, Protocol::TCP};
    vector<Thread> workers;
    atomic<bool> running{false};

public:
    Service()=default;

    ~Service(){ stop(); }

    void start(){
        cout<<"[Service] starting...\n";

        running=true;

        // 创建一个 worker
        workers.emplace_back([this]{
            while(running){
                cout<<"[Worker] working...\n";
                this_thread::sleep_for(chrono::seconds(1));
            }
            cout<<"[Worker] exit\n";
        });

        cout<<"[Service] started\n";
    }

    void run(){
        cout<<"[Service] running...\n";
        while(running){
            this_thread::sleep_for(chrono::seconds(1));
        }
    }

    void stop(){
        if(!running) return;

        cout<<"[Service] stopping...\n";
        running=false;

        workers.clear(); // Thread析构自动join
        cout<<"[Service] stopped\n";
    }
};

////////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////////
int main(){
    try{
        Service service;
        service.start();
        service.run();
        service.stop();
    }catch(const exception& e){
        cerr<<"[Fatal] "<<e.what()<<endl;
    }

    return 0;
}
