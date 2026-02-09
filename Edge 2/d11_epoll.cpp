#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<unistd.h>
#include<vector>
#include<cstring>
#include<iostream>
#include<fcntl.h>

using namespace std;

// 这是更安全的设置非阻塞式的方式
int set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    if(flags==-1){
        return -1;
    }
    return fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}

int create_listen_socket(int port){
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){
        perror("socket failed.");
        return -1;
    }

    int opt=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=INADDR_ANY;
    server_addr.sin_port=htons(8080);

    if(bind(fd,(sockaddr*)&server_addr,sizeof(server_addr))<0){
        perror("bind failed.");
        return -1;
    }

    if(listen(fd,128)<0){
        perror("listen failed.");
        return -1;
    }

    cout<<"TCP server listening on port 8080!"<<endl;
    return fd;
}

void handle_client(int client_fd){
    char buffer[1024];

    while(true){
        ssize_t n=read(client_fd,buffer,sizeof(buffer)-1);

        if(n>0){
            buffer[n]='\0';
            cout<<"[Client "<<client_fd<<"]: "<<buffer<<endl;
            write(client_fd,buffer,n);
        } 
        else if(n==0){
            cout<<"[-] Client fd = "<<client_fd<<" disconnected"<<endl;
            close(client_fd);
            break;
        }
        else{
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }
            else{
                perror("read error");
                close(client_fd);
                break;
            }
        }
    }
}

void run_epoll_server(int port){
    int listen_fd=create_listen_socket(port);
    if(listen_fd<0){
        return;
    }

    // 设置为非阻塞式
    set_nonblocking(listen_fd);
    
    // 实例化epoll对象
    int epfd=epoll_create1(0);
    if(epfd<0){
        perror("epoll create fail");
        close(listen_fd);
        return ;
    }

    epoll_event ev{};
    ev.events=EPOLLIN;
    ev.data.fd=listen_fd;
    epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev);

    vector<epoll_event>events(1024);

    while(true){
        int ready=epoll_wait(epfd,events.data(),events.size(),-1);
        if(ready<0){
            perror("epoll_wait error");
            break;
        }

        for(int i=0;i<ready;i++){
            int fd=events[i].data.fd;
            if(fd==listen_fd){
                while(true){
                    // 表示有新连接建立
                    sockaddr_in client_addr{};
                    socklen_t len=sizeof(client_addr);

                    int client_fd=accept(listen_fd,(sockaddr*)&client_addr,&len);
                    if(client_fd<0){
                        if(errno==EAGAIN||errno==EWOULDBLOCK){
                            break;
                        }
                        perror("accept failed.");
                        continue;
                    }

                    set_nonblocking(client_fd);

                    epoll_event celievent{};
                    celievent.events=EPOLLIN;
                    celievent.data.fd=client_fd;
                    epoll_ctl(epfd,EPOLL_CTL_ADD,client_fd,&celievent);

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET,&client_addr.sin_addr,ip,sizeof(ip));
                    int port=ntohs(client_addr.sin_port);

                    cout<<"[+] new client connected: "<<ip<<":"<<port<<" (fd="<<client_fd<<")"<<endl;
                }
            }
            else{
                handle_client(fd);
            }
        }
        
    }

    close(epfd);
    close(listen_fd);
}

int main(){
    run_epoll_server(8080);
    return 0;
}