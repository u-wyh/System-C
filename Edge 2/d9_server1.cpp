#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>
#include<vector>
#include<cstring>
#include<iostream>
#include<fcntl.h>

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

void handle_client(int fd) {
    char buffer[1024];
    while (true) {
        int n = read(fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            cout << "[Client " << fd << "]: " << buffer<<endl;
            write(fd, buffer, n); // 回显
        } else if (n == 0) {
            // 客户端关闭
            cout << "[-] Client fd=" << fd << " disconnected" << endl;
            close(fd);
            break;
        } else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有更多数据
                break;
            } else {
                perror("read");
                close(fd);
                break;
            }
        }
    }
}

void run_select_server(int port){
    int listen_fd=create_listen_socket(port);
    if(listen_fd<0){
        return;
    }

    // 设置为非阻塞式
    fcntl(listen_fd,F_SETFL,O_NONBLOCK);

    fd_set master_set,ready_set;
    int max_fd=listen_fd;
    FD_ZERO(&master_set);
    FD_SET(listen_fd,&master_set);

    while(true){
        ready_set=master_set;
        int ready_count=select(max_fd+1,&ready_set,nullptr,nullptr,nullptr);
        if(ready_count<0){
            perror("select error");
            break;
        }

        for(int fd=0;fd<=max_fd&&ready_count>0;fd++){
            if(FD_ISSET(fd,&ready_set)){
                ready_count--;

                if(fd==listen_fd){
                    // 表示有新连接建立
                    while(true){
                        sockaddr_in client_addr{};
                        socklen_t len=sizeof(client_addr);

                        int client_fd=accept(fd,(sockaddr*)&client_addr,&len);
                        if(client_fd<0){
                            // 如果已经建立连接的队列空了，那么直接跳出
                            if(errno==EAGAIN||errno==EWOULDBLOCK){
                                break;
                            }
                            perror("accept failed.");
                            continue;
                        }

                        fcntl(client_fd,F_SETFL,O_NONBLOCK);
                        FD_SET(client_fd,&master_set);
                        if(client_fd>max_fd){
                            max_fd=client_fd;
                        }

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
    }

    for(int fd=0;fd<=max_fd;fd++){
        if(FD_ISSET(fd,&master_set)){
            close(fd);
        }
    }

    close(listen_fd);
}

int main()
{
    run_select_server(8080);
    return 0;
}