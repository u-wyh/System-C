#include<iostream>
#include<thread>
#include<mutex>
#include<atomic>
#include<chrono>
#include<functional>

using namespace std;

mutex ioMutex;
void log_line(const string &msg) {
    lock_guard<mutex> lock(ioMutex);
    cout << msg << endl;
}

int unsafeCount=0;
atomic<int>safeCount{0};

void data_race_bug(){
    for(int i=1;i<=10000000;i++){
        unsafeCount++;
    }
}

void data_race_fix(){
    for(int i=1;i<=10000000;i++){
        safeCount++;
    }
}

mutex mutex1,mutex2;

void deadlock1(){
    lock_guard<mutex> lock1(mutex1);
    this_thread::sleep_for(chrono::milliseconds(100));
    lock_guard<mutex> lock2(mutex2);
    log_line("deadlock1 finished!");
}

void deadlock2(){
    lock_guard<mutex> lock1(mutex2);
    this_thread::sleep_for(chrono::milliseconds(100));
    lock_guard<mutex> lock2(mutex1);
    log_line("deadlock1 finished!");
}

atomic<bool>res1{false};
atomic<bool>res2{false};

void livelock(const string& name,atomic<bool>& self,atomic<bool>& other){
    while(true){
        self.exchange(true);
        this_thread::sleep_for(chrono::milliseconds(10));
        if(!other.load()){
            log_line(name+" task finished!");
            self=false;
            break;
        }
        else{
            log_line(name+" backing off(relaseing self).");
            self=false;
        }
    }
}

int main()
{
    // 数据竞争
    log_line("data race:");
    thread t1(data_race_bug);
    thread t2(data_race_bug);
    thread t3(data_race_bug);
    thread t4(data_race_bug);
    t1.join();t2.join();t3.join();t4.join();
    log_line("unsafecount = "+to_string(unsafeCount));

    thread t5(data_race_fix);
    thread t6(data_race_fix);
    thread t7(data_race_fix);
    thread t8(data_race_fix);
    t5.join();t6.join();t7.join();t8.join();
    log_line("safecount = "+to_string(safeCount));
    log_line("");

    // 死锁
    log_line("deadlock:");
    thread d1(deadlock1);
    thread d2(deadlock2);
    this_thread::sleep_for(chrono::seconds(2));
    log_line("deadlock occured!");
    log_line("");

    // 活锁
    log_line("livelock:");
    thread l1(livelock,"Thread-A",ref(res1),ref(res2));
    thread l2(livelock,"Thread-B",ref(res2),ref(res1));
    l1.detach(),l2.detach();
    this_thread::sleep_for(chrono::seconds(1));
    log_line("");
    return 0;
}