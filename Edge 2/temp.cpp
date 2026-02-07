#include<iostream>
#include<vector>
#include<thread>
#include<mutex>
#include<atomic>
#include<chrono>
#include<queue>
#include<functional>
#include<condition_variable>

using namespace std;

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
                        cerr<<"[Thread "<<i<<"] exception: "<<e.what()<<endl;
                    }
                    catch(...){
                        cerr<<"[Thread "<<i<<"] unknown exception."<<endl;
                    }
                }
            });
        }
    }

    ~ThreadPool(){
        shutdown();
    }

    void submit(function<void()>task){
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

int main()
{
    ThreadPool pool(4);

    for(int i=1;i<=20;i++){
        pool.submit([i](){
            cout<<"Task "<<i<<" started,thread id : "<<this_thread::get_id()<<endl;
            this_thread::sleep_for(chrono::seconds(1));
            cout<<"Task "<<i<<" end\n";
        });
    }

    this_thread::sleep_for(chrono::seconds(10));

    pool.shutdown();

    return 0;
}