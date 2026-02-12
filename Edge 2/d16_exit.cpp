#include<iostream>
#include<vector>
#include<thread>
#include<mutex>
#include<atomic>
#include<chrono>
#include<queue>
#include<functional>
#include<condition_variable>
#include<sstream>
#include<fstream>
#include<iomanip>
#include<random>
#include<algorithm>
#include<csignal>

using namespace std;

// 线程输出函数，避免输出混乱
mutex ioMutex;

void log_line(const string& msg){
    lock_guard<mutex> lock(ioMutex);
    cout<<msg<<endl;
}

// 获取当前时间
string now_time(){
    auto now=chrono::system_clock::now();
    time_t t=chrono::system_clock::to_time_t(now);
    tm tm_buf{};
    localtime_r(&t, &tm_buf); // Linux 下线程安全
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return string(buf);
}

enum class TaskType{
    COMPUTE,IO
};

struct Task{
    TaskType type;
    string name;
    function<void()>func;

    Task(TaskType t,const string&n,function<void()>f):
        type(t),name(n),func(f){}
};

class ThreadPool{

private:
    // 线程池
    vector<thread>workers;
    // 任务列表，先进先出
    queue<Task>tasks;

    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running;

    // 统计信息，用于判断负载是否均衡
    vector<int>threadTask;
    atomic<int>totalException;

public:
    ThreadPool(size_t threadNum):running(true),threadTask(threadNum,0),totalException(0){
        for(size_t i=0;i<threadNum;i++){
            workers.emplace_back([this,i](){
                while(true){
                    // 构造一个临时变量，用于后面的move
                    Task task(TaskType::IO,"",[]{});
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

                    auto start=chrono::high_resolution_clock::now();
                    try{
                        string msg="["+now_time()+"][Thread "+to_string(i)+"] exceuting task: "+task.name+" ["+(task.type==TaskType::IO?"IO":"COMPUTE")+"]";
                        log_line(msg);
                        task.func();
                    }
                    catch(const exception& e){
                        string msg="["+now_time()+"][Thread "+to_string(i)+"] exception: "+e.what();
                        log_line(msg);
                        totalException++;
                    }
                    catch(...){
                        string msg="["+now_time()+"][Thread "+to_string(i)+"] unknown exception: ";
                        log_line(msg);
                        totalException++;
                    }
                    auto end=chrono::high_resolution_clock::now();
                    auto duration=chrono::duration_cast<chrono::milliseconds>(end - start).count();
                    threadTask[i]++;
                    string msg="[Thread "+to_string(i)+"] task completed, duration: "+to_string(duration)+" ms";
                    log_line(msg);
                }
            });
        }
    }

    ~ThreadPool(){
        shutdown();
    }

    void submit(Task task){
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
        log_line("ThreadPool shutdown.");
        log_line("Total exceptions: "+to_string(totalException));
        for (size_t i = 0; i < threadTask.size(); ++i) {
            log_line("Thread "+to_string(i)+" executed "+to_string(threadTask[i])+" tasks");
        }
    }
};

class Service{

public:
    enum class State{
        INIT,RUNNING,STOPPING,STOPPED
    };

    Service(size_t threadNum):state(State::INIT),pool(threadNum){}

    ~Service(){
        if (get_state()==State::RUNNING||get_state()==State::STOPPING) {
            stop();
        }
    }

    void start(){
        lock_guard<mutex> lock(stateMutex);
        if(state!=State::INIT){
            log_line("Service cannot start,current state: "+state_to_string(state));
            return ;
        }
        state=State::RUNNING;
        log_line("Service starting...");

        mainThread=thread([this](){
            int counter=0;
            while(true){
                {
                    lock_guard<mutex> lock(stateMutex);
                    if(state!=State::RUNNING){
                        break;
                    }
                }

                pool.submit(Task(TaskType::COMPUTE,"Task"+to_string(counter),[counter](){
                    log_line("Executing Task "+to_string(counter));
                    this_thread::sleep_for(chrono::milliseconds(200));
                }));

                counter++;
                this_thread::sleep_for(chrono::milliseconds(500));
            }
            log_line("Service main thread exiting...");
            lock_guard<mutex> lock(stateMutex);
            state =State::STOPPED;
        });
    }

    void stop(){
        {
            lock_guard<mutex> lock(stateMutex);
            if(state!=State::RUNNING){
                log_line("Service not running,cannot stop");
                return ;
            }
            log_line("Service stopping...");
            state=State::STOPPING;
        }

        if(mainThread.joinable()){
            mainThread.join();
        }
        pool.shutdown();
        log_line("Service stopped.");
    }

    State get_state(){
        lock_guard<mutex> lock(stateMutex);
        return state;
    }

    static string state_to_string(State s){
        switch(s){
            case State::INIT : return "INIT";
            case State::RUNNING : return "RUNNING";
            case State::STOPPING : return "STOPPING";
            case State::STOPPED : return "STOPPED";
            default : return "UNKNOWN";
        }
    }

private:
    mutex stateMutex;
    State state;
    ThreadPool pool;
    thread mainThread;
};

Service *g_service=nullptr;

void signal_handler(int signal){
    if(g_service){
        log_line("Signal received: "+to_string(signal));
        g_service->stop();
    }
}

int main()
{
    Service service(4);
    g_service=&service;
    signal(SIGINT,signal_handler);
    signal(SIGTERM,signal_handler);
    log_line("Initial state: "+Service::state_to_string(service.get_state()));

    service.start();
    this_thread::sleep_for(chrono::seconds(10));

    if(service.get_state()==Service::State::RUNNING){
        service.stop();
    }
    log_line("Final state: "+Service::state_to_string(service.get_state()));
    return 0;
}