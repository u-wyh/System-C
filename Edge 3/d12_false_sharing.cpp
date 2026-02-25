#include<iostream>
#include<thread>
#include<vector>
#include<chrono>
#include<pthread.h>

using namespace std;

const int THREADS = 4;
const int ITER = 500000000;

int val[THREADS];

void bind_to_core(int core_id){
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id,&cpuset);
    pthread_setaffinity_np(pthread_self(),sizeof(cpu_set_t),&cpuset);
}

void worker(int id){
    bind_to_core(id);
    for(int i=0;i<ITER;i++){
        val[id]++;
    }
}

int main()
{
    auto start=chrono::high_resolution_clock::now();

    vector<thread>threads;
    for(int i=0;i<THREADS;i++){
        threads.emplace_back(worker,i);
    }
    for(auto& t:threads){
        t.join();
    }

    auto end=chrono::high_resolution_clock::now();

    cout<<"Time cost: "<<chrono::duration<double>(end-start).count()<<" s"<<endl;
    return 0;
}