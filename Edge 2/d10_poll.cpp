#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>
#include<vector>
#include<cstring>
#include<iostream>
#include<fcntl.h>
#include<poll.h>
#include<algorithm>

using namespace std;

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

void run_poll_server(int port){
    int listen_fd=create_listen_socket(port);
    if(listen_fd<0){
        return;
    }

    // 设置为非阻塞式
    fcntl(listen_fd,F_SETFL,O_NONBLOCK);

    vector<pollfd>fds;
    fds.push_back({listen_fd,POLLIN,0});

    while(true){
        int ret=poll(fds.data(),fds.size(),-1);
        if(ret<0){
            perror("poll error");
            break;
        }

        for(size_t i=0;i<fds.size()&&ret>0;i++){
            if(fds[i].revents&POLLIN){
                ret--;

                if(fds[i].fd==listen_fd){
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

                        fcntl(client_fd,F_SETFL,O_NONBLOCK);
                        fds.push_back({client_fd,POLLIN,0});

                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET,&client_addr.sin_addr,ip,sizeof(ip));
                        int port=ntohs(client_addr.sin_port);

                        cout<<"[+] new client connected: "<<ip<<":"<<port<<" (fd="<<client_fd<<")"<<endl;
                    }
                }
                else{
                    int fd=fds[i].fd;
                    handle_client(fd);
                    if(fcntl(fd,F_GETFL)==-1){
                        fds[i].fd=-1;
                    }
                }
            }
        }

        fds.erase(remove_if(fds.begin(),fds.end(),[](const pollfd &p){return p.fd<0;}),fds.end());
    }

    for(auto &pfd:fds){
        if(pfd.fd>=0){
            close(pfd.fd);
        }
    }

    close(listen_fd);
}

int main(){
    run_poll_server(8080);
    return 0;
}