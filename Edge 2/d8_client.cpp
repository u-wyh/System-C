#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>
#include<chrono>
#include<cstring>
#include<iostream>
#include<thread>
#include<vector>
#include<string>

using namespace std;

void client_worker(const string &server_ip,int server_port,int id){
    int sockfd=socket(AF_INET,SOCK_STREAM,0);
    if(sockfd<0){
        perror("socket failed.");
        return ;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(server_port);
    inet_pton(AF_INET,server_ip.c_str(),&server_addr.sin_addr);

    if(connect(sockfd,(sockaddr*)&server_addr,sizeof(server_addr))<0){
        perror("connect error.");
        close(sockfd);
        return ;
    }

    cout<<"[client "<<id<<"] connected."<<endl;

    while(true){
        const char *msg="hello world\n";
        send(sockfd,msg,strlen(msg),0);
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    close(sockfd);
}

int main()
{
    int num=200;
    string server_ip="127.0.0.1";
    int server_port=8080;

    vector<thread>threads;
    threads.reserve(num);

    for(int i=1;i<=num;i++){
        threads.emplace_back(client_worker,server_ip,server_port,i);
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    for(auto& t:threads){
        t.join();
    }

    return 0;
}