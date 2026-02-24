#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sched.h>

using namespace std;

std::atomic<bool> running(true);
atomic<int> cnt{0};

void set_affinity(int thread_id, int core_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t thread = pthread_self();
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
}

void cpu_worker(int id)
{
    // int core_id=id%4;
    // set_affinity(id,core_id);
    while (running)
    {
        cnt++;
        // 模拟 CPU 密集型计算
        volatile double x = 0;
        for (int i = 0; i < 10000000; ++i)
        {
            x += i * 0.000001;
        }

        // 获取当前运行的 CPU 核心
        int cpu = sched_getcpu();

        std::cout << "Thread " << id
                  << " running on CPU " << cpu
                  << " (TID=" << gettid() << ")"
                  << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int main()
{
    int thread_count = 16;

    std::vector<std::thread> threads;

    std::cout << "Starting " << thread_count << " threads\n";
    std::cout << "PID: " << getpid() << std::endl;

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back(cpu_worker, i);
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));
    running = false;

    for (auto& t : threads)
    {
        t.join();
    }

    std::cout << "Finished.\n";
    cout<<cnt<<endl;
    return 0;
}
// taskset -c 0 ./d9_process
// perf stat -e context-switches ./d9_process