#include<arpa/inet.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<unistd.h>
#include<fcntl.h>
#include<vector>
#include<cstring>
#include<iostream>
#include<queue>
#include<thread>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<atomic>
#include<chrono>
#include<sstream>

using namespace std;


// 线程输出函数，避免输出混乱
mutex ioMutex;

void log_line(const string& msg){
    lock_guard<mutex> lock(ioMutex);
    cout<<msg<<endl;
}

// 这是更安全的设置非阻塞式的方式
int set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    if(flags==-1){
        return -1;
    }
    return fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}

class ThreadPool{

private:
    // 线程池
    vector<thread>workers;
    // 任务列表，先进先出
    queue<function<void()>>tasks;

    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running;

public:
    ThreadPool(size_t threadNum):running(true){
        for(size_t i=0;i<threadNum;i++){
            workers.emplace_back([this,i](){
                while(running){
                    // task是个临时变量，结束后自动析构，workers线程可以接受新的任务
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        // 要么被显式解锁(notify_one/all)  要么被系统解锁，就是这个函数
                        condvar.wait(lock,[this](){
                            return !tasks.empty()||!running;
                        });
                        if(!running&&tasks.empty()){
                            return ;
                        }
                        task=move(tasks.front());
                        tasks.pop();
                    }

                    try{
                        task();
                    }
                    catch(const exception& e){
                        log_line("[Worker] exception: "+string(e.what()));
                    }
                    catch(...){
                        log_line("[Worker] unknown exception");
                    }
                }
            });
        }
    }

    ~ThreadPool(){
        shutdown();
    }

    void submit(function<void()>task){
        if (!running) {
            throw runtime_error("ThreadPool is stopped, cannot submit task.");
        }
        {
            lock_guard<mutex>lock(queueMutex);
            tasks.push(move(task));
        }
        condvar.notify_one();
    }

    void shutdown(){
        running=false;
        condvar.notify_all();
        for(auto& t:workers){
            if(t.joinable()){
                t.join();
            }
        }
    }
};

class EpollServer{

private:
    int listen_fd;
    int epfd;
    ThreadPool &pool;

public:
    EpollServer(int port,ThreadPool &tp):pool(tp){
        listen_fd=socket(AF_INET,SOCK_STREAM,0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
        listen(listen_fd, 128);

        set_nonblocking(listen_fd);
        epfd = epoll_create1(0);

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        log_line("[Server] listening on port "+to_string(port));
    }

    void run(){
        vector<epoll_event>events(1024);
        while(true){
            int n=epoll_wait(epfd,events.data(),events.size(),-1);
            for(int i=0;i<n;i++){
                int fd=events[i].data.fd;
                if(fd==listen_fd){
                    accept_client();
                }
                else{
                    handle_client(fd);
                }
            }
        }
    }

    void accept_client(){
        while (true){
            sockaddr_in client{};
            socklen_t len=sizeof(client);
            int cfd=accept(listen_fd,(sockaddr*)&client,&len);
            if(cfd<0){
                if(errno==EAGAIN){
                    break;
                }
                perror("accept fail");
                continue;
            }
            set_nonblocking(cfd);

            epoll_event ev{};
            ev.events=EPOLLIN;
            ev.data.fd=cfd;
            epoll_ctl(epfd,EPOLL_CTL_ADD,cfd,&ev);

            log_line("[+] new client fd="+to_string(cfd));
        }
    }

    void handle_client(int fd){
        char buffer[1024];

        while(true){
            ssize_t n=read(fd,buffer,sizeof(buffer)-1);

            if(n>0){
                buffer[n]='\0';
                string data(buffer);
                log_line("[Reactor "+to_string(fd)+"] received:"+data);
                pool.submit([fd,data](){
                    stringstream ss;
                    ss<<this_thread::get_id();
                    log_line("[Worker] thread="+ss.str()+" start proccesing fd = "+to_string(fd));
                });
                write(fd,buffer,n);
            } 
            else if(n==0){
                cout<<"[-] Client fd = "<<fd<<" disconnected"<<endl;
                close(fd);
                break;
            }
            else{
                if(errno==EAGAIN||errno==EWOULDBLOCK){
                    break;
                }
                else{
                    perror("read error");
                    close(fd);
                    break;
                }
            }
        }
    }
};

int main(){
    ThreadPool pool(4);
    EpollServer server(8080,pool);
    server.run();
    return 0;
}