#include<bits/stdc++.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
using namespace std;

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
        ::listen(fd,backlog);
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

int main()
{
    try{
        Socket udp(8080,Protocol::UDP);
        // udp.listen_tcp();
        Socket tcp(8080,Protocol::TCP);
        tcp.listen_tcp();

        int client_fd=tcp.accept_tcp();
        string msg;
        tcp.recv_tcp(client_fd,msg);
        cout<<"[TCP] received : "<<msg<<endl;
        tcp.send_tcp(client_fd,msg);
        ::close(client_fd);

        sockaddr_in client_addr{};
        udp.recv_udp(msg,&client_addr);
        cout<<"[UDP] received :"<<msg<<endl;
        udp.send_udp(msg,client_addr);
        // echo "hello UDP" | nc -u 127.0.0.1 8080

        try{
            Socket tcpfail(8080,Protocol::TCP);
        }
        catch(const exception& e){
            cerr<<"[Error] "<<e.what()<<endl;
        }

        Socket tcp2=move(tcp);
    }
    catch(const exception& e){
        cerr<<"[Fatal] "<<e.what()<<endl;
    }
    return 0;
}