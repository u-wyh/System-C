#include<iostream>
#include<thread>
#include<vector>
#include<chrono>
#include<pthread.h>

using namespace std;

const int THREADS = 4;
const int ITER = 500000000;

int val[THREADS];

int main()
{
    auto start=chrono::high_resolution_clock::now();

    for(int j=0;j<ITER;j++){
        for(int i=0;i<THREADS;i++){
            val[i]++;
        }
    }

    auto end=chrono::high_resolution_clock::now();

    cout<<"Time cost: "<<chrono::duration<double>(end-start).count()<<" s"<<endl;
    return 0;
}