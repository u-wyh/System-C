#include<bits/stdc++.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
using namespace std;

class File{

private:
    FILE* fptr;

public:
    File(const string& filename,const string& mode):fptr(nullptr){
        fptr=fopen(filename.c_str(),mode.c_str());
        if(!fptr){
            cerr<<"Failed to open file : "<<filename<<endl;
            fptr=nullptr;
            // 即使构造失败，也不直接exit
        }
    }

    // 禁止拷贝，无论是深拷贝还是浅拷贝
    File(const File&)=delete;
    File& operator=(const File&)=delete;

    // 支持移动
    File(File&& other) noexcept:fptr(other.fptr){
        cout<<"Move Constructor"<<endl;
        other.fptr=nullptr;
    }

    File& operator=(File&& other) noexcept{
        cout<<"Move Assignment"<<endl;
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
        if(!fptr){
            return false;
        }
        return fwrite(data.c_str(),1,data.size(),fptr)==data.size();
    }

    string read_line(){
        if(!fptr){
            return "";
        }
        char buffer[1024];
        if(fgets(buffer,sizeof(buffer),fptr)){
            return string(buffer);
        }
        return "";
    }

    void seek_to_begin(){
        if(fptr){
            fseek(fptr,0,SEEK_SET);
        }
    }

    void flush(){
        if(fptr){
            fflush(fptr);
        }
    }

private:
    void close(){
        if(fptr){
            cout<<"file close"<<endl;
            fclose(fptr);
            fptr=nullptr;
        }
    }

};

enum class Protocol{
    TCP,UDP
};

class Socket{

private:
    int fd;
    int port;
    Protocol protocol;

    void close(){
        cout<<"bye "<<port<<endl;
        if(fd!=-1){
            // 系统调用，使用全局API
            ::close(fd);
            cout<<"[Socket] closed "<<get_protocol()<<" socket on port"<<port<<endl;
            fd=-1;
        }
    }

public:
    Socket(int port_,Protocol protocol_):fd(-1),port(port_),protocol(protocol_){
        int type=(protocol==Protocol::TCP)?SOCK_STREAM:SOCK_DGRAM;
        fd=::socket(AF_INET,type,0);
        if(fd==-1){
            // 如果有问题，try-catch会打印问题所在
            throw runtime_error("socket() failed: "+string(strerror(errno)));
        }

        // 准备设置ip地址和端口信息，这里是ipv4
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=INADDR_ANY;
        addr.sin_port=htons(port);

        // 获取文件描述符,绑定端口
        if(::bind(fd,(sockaddr*)&addr,sizeof(addr))<0){
            // 首先取消掉原有fd可能存在的资源
            ::close(fd);
            fd=-1;

            throw runtime_error("bind() failed on port "+to_string(port)+" : "+string(strerror(errno)));
        }

        cout<<"[Socket] created "<<get_protocol()<<" socket on port "<<port<<endl;
    }

    // 禁止拷贝
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;

    // 允许移动
    Socket(Socket&& other)noexcept:fd(other.fd),port(other.port),protocol(other.protocol){
        other.fd=-1;
    }
    Socket& operator=(Socket&& other)noexcept{
        if(this!=&other){
            close();
            fd=other.fd;
            port=other.port;
            protocol=other.protocol;
            other.fd=-1;
        }
        return *this;
    }

    ~Socket(){
        close();
    }

    string get_protocol(){
        return (protocol==Protocol::TCP)?"TCP":"UDP";
    }

    // TCP功能
    void listen_tcp(int backlog=5){
        // 默认最多可以接受5个半连接状态
        if(protocol!=Protocol::TCP){
            throw runtime_error("Not a TCP socket");
        }
        // 这里进行了修改，系统编程中 不检查系统调用 = 默认失败
        if(::listen(fd,backlog)<0){
            throw runtime_error("listen() failed");
        }
        
        cout<<"[TCP] listening on port "<<port<<endl;
    }

    int accept_tcp(){
        sockaddr_in client_addr{};
        socklen_t len=sizeof(client_addr);
        int client_fd=::accept(fd,(sockaddr*)&client_addr,&len);
        if(client_fd==-1){
            throw runtime_error("accept() failed");
        }
        cout<<"[TCP] client connected from "<<inet_ntoa(client_addr.sin_addr)<<":"<<ntohs(client_addr.sin_port)<<endl;
        return client_fd;
    }

    ssize_t recv_tcp(int client_fd,string& str,size_t maxlen=1024){
        char buffer[1024];
        ssize_t n=::recv(client_fd,buffer,maxlen-1,0);
        if(n>0){
            buffer[n]=0;
            str=buffer;
        }
        return n;
    }

    ssize_t send_tcp(int client_fd,const string &str){
        return ::send(client_fd,str.c_str(),str.size(),0);
    }

    // UDP功能
    ssize_t recv_udp(string &str,sockaddr_in *client_addr=nullptr){
        char buffer[1024];
        sockaddr_in addr{};
        socklen_t len=sizeof(addr);
        ssize_t n=::recvfrom(fd,buffer,sizeof(buffer)-1,0,(client_addr?(sockaddr*)client_addr:(sockaddr*)&addr),&len);
        if(n>0){
            buffer[n]=0;
            str=buffer;
        }
        return n;
    }

    ssize_t send_udp(const string &str,const sockaddr_in &client_addr){
        return ::sendto(fd,str.c_str(),str.size(),0,(const sockaddr*)&client_addr,sizeof(client_addr));
    }
};

// 互斥锁类
class Mutex{

private:
    mutex a_mutex;
    friend class LockGuard;

public:
    // 默认构造函数，不上锁
    Mutex()=default;
    ~Mutex()=default;

    // 禁止拷贝
    Mutex(const Mutex&)=delete;
    Mutex& operator=(const Mutex&)=delete;

    // 支持移动
    Mutex(Mutex&&)=default;
    Mutex& operator=(Mutex&&)=default;

    // 阻塞式
    void lock(){
        a_mutex.lock();
    }

    void unlock(){
        a_mutex.unlock();
    }

    // 非阻塞式
    bool try_lock(){
        return a_mutex.try_lock();
    }
};

// 管理互斥锁类
class LockGuard{

private:
    Mutex& a_mutex;

public:
    LockGuard(Mutex& mutex):a_mutex(mutex){
        a_mutex.lock();
    }
    ~LockGuard(){
        a_mutex.unlock();
    }

    LockGuard(const LockGuard&)=delete;
    LockGuard& operator=(const LockGuard&)=delete;
};

class Thread{

private:
    thread t;

public:
    template<typename Func,typename... Args>
    explicit Thread(Func&& f,Args&&... args){
        try{
            t=thread(forward<Func>(f),forward<Args>(args)...);
        }
        catch(const system_error&e){
            cerr<<"Thread creation failed: "<<e.what()<<endl;
            throw;
        }
    }

    ~Thread(){
        if(t.joinable()){
            cout<<"[Thread] is over!"<<endl;
            t.join();
        }
    }

    Thread(const Thread&)=delete;
    Thread& operator=(const Thread&)=delete;

    // 这里的移动函数只能借助系统提供的函数实现，因为我们没有权限
    Thread(Thread&& other)noexcept:t(move(other.t)){}

    Thread& operator=(Thread&& other)noexcept{
        if(this!=&other){
            if(t.joinable()){
                t.join();
            }
            // 这个地方进行了修改完善
            t=move(other.t);
            return *this;
        }
    }
};

class Service{

private:
    File logFile{"/root/service.log","w"};
    Socket tcp{8080,Protocol::TCP};
    // Socket tcp2{8080,Protocol::TCP};
    vector<Thread>workers;
    atomic<bool>running{false};

public:
    Service()=default;
    ~Service()=default;

    void log_service(){
        if(!logFile.is_valid()){
            cerr<<"[Log] disabled"<<endl;
            return ;
        }

        int count=0;
        while(running){
            logFile.write("[Worker] log entry"+to_string(count++)+"\n");
            logFile.flush();
            this_thread::sleep_for(chrono::seconds(1));
        }
    }

    void tcp_service(){
        while(running){
            int client_fd=-1;
            try{
                client_fd=tcp.accept_tcp();
                string msg;
                while(tcp.recv_tcp(client_fd,msg)>0){
                    tcp.send_tcp(client_fd,msg);
                    if(rand()%5==0){
                        ::close(client_fd);
                        throw runtime_error("random tcp failure");
                    }
                }
            }
            catch(const exception &e){
                cerr<<"[TCP error] "<<e.what()<<endl;
            }
            // if(client_fd!=-1){
            //     ::close(client_fd);
            // }
        }        
    }

    void shutdown(){
        this_thread::sleep_for(chrono::seconds(5));
        running=false;
    }

    void start(){
        running=true;

        tcp.listen_tcp();

        try{
            workers.emplace_back(&Service::log_service,this);
            workers.emplace_back(&Service::tcp_service,this);
            workers.emplace_back(&Service::shutdown,this);
        }
        catch(...){
            running=false;
            workers.clear();
            throw;
        }
        cout<<"[Service] started"<<endl;
    }

    void run(){
        // int now=0;
        while(running){
            this_thread::sleep_for(chrono::milliseconds(5));
            // cout<<now<<endl;
            // now++;
        }
        stop();
    }

    void stop(){
        if(!running){
            return ;
        }
        running=false;
        workers.clear();
        cout<<"[Service] stopped"<<endl;
    }
};

int main()
{
    try{
        Service service;
        service.start();
        service.run();
    }
    catch(const exception& e){
        cerr<<"[Fatal] "<<e.what()<<endl;
    }
    return 0;
}