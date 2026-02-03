#include<bits/stdc++.h>
#include<sys/socket.h>
#include<netinet/in.h>
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

        cout<<"[Socket] created"<<get_protocol()<<" socket on port "<<port<<endl;
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
};

int main()
{
    try{
        Socket udp(8080,Protocol::UDP);
        Socket tcp(8080,Protocol::TCP);

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