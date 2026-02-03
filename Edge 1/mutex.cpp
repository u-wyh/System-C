#include<bits/stdc++.h>
using namespace std;

// 互斥锁类
class Mutex{

private:
    mutex a_mutex;
    friend class LockGuard;

public:
    // 默认构造函数，不上锁
    Mutex()=default;
    ~Mutex()=default;

    // 禁止拷贝
    Mutex(const Mutex&)=delete;
    Mutex& operator=(const Mutex&)=delete;

    // 支持移动
    Mutex(Mutex&&)=default;
    Mutex& operator=(Mutex&&)=default;

    // 阻塞式
    void lock(){
        a_mutex.lock();
    }

    void unlock(){
        a_mutex.unlock();
    }

    // 非阻塞式
    bool try_lock(){
        return a_mutex.try_lock();
    }
};

// 管理互斥锁类
class LockGuard{

private:
    Mutex& a_mutex;

public:
    LockGuard(Mutex& mutex):a_mutex(mutex){
        a_mutex.lock();
    }
    ~LockGuard(){
        a_mutex.unlock();
    }

    LockGuard(const LockGuard&)=delete;
    LockGuard& operator=(const LockGuard&)=delete;
};

int counter=0;
Mutex a_mutex;

void worker(int id){
    for(int i=0;i<5;i++){
        // 在锁之前，先关闭1秒，这样可以展示出竞争
        this_thread::sleep_for(chrono::seconds(1));
        LockGuard lock(a_mutex);
        ++counter;
        cout<<"worker "<<id<<": "<<counter<<" now"<<endl;
    }
}

Mutex m;

void show(int id){
    if(m.try_lock()){
        cout<<"Thread "<<id<<" get the lock"<<endl;
        this_thread::sleep_for(chrono::seconds(1));
        m.unlock();// 手动解锁
    }
    else{
        cout<<"Thread "<<id<<" not get the lock"<<endl;
    }
}


Mutex mutex1,mutex2;

void deadlock1(int id){
    LockGuard lock1(mutex1);
    // 这个时间设置一定要合理，太短的话就不会导致死锁
    this_thread::sleep_for(chrono::milliseconds(100));
    LockGuard lock2(mutex2);
    cout<<"Thread "<<id<<" finish."<<endl;
}

void deadlock2(int id){
    LockGuard lock2(mutex2);
    this_thread::sleep_for(chrono::milliseconds(100));
    LockGuard lock1(mutex1);
    cout<<"Thread "<<id<<" finish."<<endl;
}

// 用于在线程之间传递信息
condition_variable cv;
mutex cv_mutex;
bool ready=false;

void waiting(){
    unique_lock<mutex>lock(cv_mutex);
    cout<<"waiting for ready"<<endl;
    // 只有ready为true是才能执行，否则等待
    cv.wait(lock,[]{return ready;});
    cout<<"waiting over"<<endl;
}

void notifying(){
    this_thread::sleep_for(chrono::seconds(1));
    // 设置一个更小的作用域，方便结束锁，因为这里的这个不能手动解锁
    {
        lock_guard<mutex>lock(cv_mutex);
        ready=true;
        cout<<"ready is true"<<endl;
    }
    // 唤醒 一个 被阻塞的线程
    cv.notify_one();
}

int main()
{
    // 两个线程共享资源，抢占式  使用lock阻塞式
    thread t1(worker,1);
    thread t2(worker,2);
    t1.join(),t2.join();
    // t1.detach(),t2.detach();
    cout<<"counter = "<<counter<<endl;

    // 两个线程共享资源，使用try_lock非阻塞式
    thread t3(show,3);
    thread t4(show,4);
    t3.join(),t4.join();

    // 死锁演示，注意挂起
    thread d1(deadlock1,5);
    thread d2(deadlock2,6);
    // 这里如果加入join，就会导致程序永久挂起
    // d1.join(),d2.join();

    // 通过条件变量控制线程
    thread w(waiting);
    thread n(notifying);
    w.join(),n.join();
    return 0;
}