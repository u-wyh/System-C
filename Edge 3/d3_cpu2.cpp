#include<arpa/inet.h>
#include<unistd.h>
#include<sys/epoll.h>
#include<fcntl.h>
#include<signal.h>
#include<iostream>
#include<thread>
#include<vector>
#include<atomic>
#include<chrono>

using namespace std;

atomic<long long>total_requests{0};
atomic<int>active_connections{0};
atomic<bool>g_running{true};

void signal_handler(int){
    g_running=false;
}

int set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    if(flags==-1){
        return -1;
    }
    return fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}

class Worker{

private:
    int epfd;

public:
    Worker(){
        epfd=epoll_create1(0);
    }

    void add_fd(int fd){
        epoll_event ev{};
        ev.events=EPOLLIN|EPOLLET;
        ev.data.fd=fd;
        epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&ev);

        active_connections++;
    }


    void run(){
        epoll_event events[1024];
        
        while(g_running){
            int n=epoll_wait(epfd,events,1024,1000);
            for(int i=0;i<n;i++){
                int fd=events[i].data.fd;
                handle_read(fd);
            }
        }

        close(epfd);
    }

    void handle_read(int fd){
        char buf[4096];

        while(true){
            ssize_t n=recv(fd,buf,sizeof(buf),0);

            if(n>0){
                total_requests++;
                send(fd,buf,n,0);
            }
            else if(n==0){
                close(fd);
                active_connections--;
                break;
            }
            else{
                if(errno==EAGAIN||errno==EWOULDBLOCK){
                    break;
                }
                close(fd);
                active_connections--;
                break;
            }
        }
    }
};

int main()
{
    signal(SIGINT,signal_handler);
    signal(SIGTERM,signal_handler);

    int port=8080;
    int workernum=4;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    set_nonblocking(listen_fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 1024);

    cout << "[Server] listening on port " << port << endl;

    vector<Worker>workers;
    vector<thread>threads;

    for(int i=1;i<=workernum;i++){
        workers.emplace_back(Worker());
    }
    for(int i=0;i<workernum;i++){
        threads.emplace_back(&Worker::run,&workers[i]);
    }

    thread monitor([](){
        long long last_total=0;
        int time=0;

        while(g_running){
            this_thread::sleep_for(chrono::seconds(1));
            long long now=total_requests.load();
            long long tps=now-last_total;
            cout<<"[Monitor] "<<++time<<" : total request="<<now<<" TPS="<<tps<<" active connections="<<active_connections.load()<<endl;

            last_total=now;
        }
    });

    int id=0;
    while(g_running){
        sockaddr_in client{};
        socklen_t len=sizeof(client);
        int cfd=accept(listen_fd,(sockaddr*)&client,&len);
        if(cfd<0){
            if(errno==EAGAIN){
                continue;
            }
            perror("accept fail");
            continue;
        }
        set_nonblocking(cfd);

        workers[id].add_fd(cfd);
        id=(id+1)%workernum;
    }
    cout<<"shutting down..."<<endl;

    close(listen_fd);

    for(auto &t:threads){
        if(t.joinable()){
            t.join();
        }
    }
    if(monitor.joinable()){
        monitor.join();
    }
    cout<<"[Server] exited gracefully."<<endl;
    return 0;
}
