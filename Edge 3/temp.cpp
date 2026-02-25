#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <pthread.h>

constexpr int THREADS = 4;
constexpr long long ITER = 500000000;

struct alignas(64) PaddedInt {
    int value;
};

struct Counter {
    PaddedInt value[THREADS];
};

Counter counter;

void bind_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void worker(int id) {
    bind_to_core(id);

    for (long long i = 0; i < ITER; ++i) {
        counter.value[id].value++;
    }
}

int main() {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads)
        t.join();

    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "Time: "
              << std::chrono::duration<double>(end - start).count()
              << " s\n";
}