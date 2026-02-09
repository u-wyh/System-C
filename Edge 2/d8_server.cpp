#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>
#include<chrono>
#include<cstring>
#include<iostream>
#include<thread>

using namespace std;

void handle_client(int client_fd,sockaddr_in client_addr){
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,&client_addr.sin_addr,ip,sizeof(ip));
    int port=ntohs(client_addr.sin_port);

    cout<<"[+] new client connected: "<<ip<<":"<<port<<" thread_id="<<this_thread::get_id()<<endl;

    char buffer[1024];

    while (true) {
        memset(buffer,0,sizeof(buffer));

        ssize_t n=recv(client_fd,buffer,sizeof(buffer),0);

        if(n>0){
            cout<<"[recv] from "<<ip<<":"<<port<<" => "<<buffer<<endl;
            send(client_fd,buffer,n,0);
        } 
        else{
            cout<<"[-] Client disconnected: "<<ip<<":"<<port<<endl;
            break;
        }
    }

    close(client_fd);
    cout<<"[thread exit] "<<ip<< ":" <<port<<endl;
}

int main()
{
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){
        perror("socket failed.");
        return 1;
    }

    int opt=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=INADDR_ANY;
    server_addr.sin_port=htons(8080);

    if(bind(fd,(sockaddr*)&server_addr,sizeof(server_addr))<0){
        perror("bind failed.");
        return 1;
    }

    if(listen(fd,128)<0){
        perror("listen failed.");
        return 1;
    }

    cout<<"TCP server listening on port 8080!"<<endl;
    while(true){
        sockaddr_in client_addr{};
        socklen_t len=sizeof(client_addr);

        int client_fd=accept(fd,(sockaddr*)&client_addr,&len);
        if(client_fd<0){
            perror("accept failed.");
            continue;
        }

        thread t(handle_client,client_fd,client_addr);
        t.detach();
    }
    return 0;
}