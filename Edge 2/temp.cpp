#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>
#include <condition_variable>
#include <chrono>
#include <sstream>
#include <iomanip>

using namespace std;
using namespace std::chrono;

// -------------------------
// 线程安全输出函数
// -------------------------
mutex ioMutex;
void log_line(const string& msg) {
    lock_guard<mutex> lock(ioMutex);
    cout << msg << endl;
}

// -------------------------
// 获取当前时间字符串（Linux 下线程安全）
// -------------------------
string now_datetime() {
    auto now = system_clock::now();
    time_t t = system_clock::to_time_t(now);
    tm tm_buf{};
    localtime_r(&t, &tm_buf); // Linux 下线程安全
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return string(buf);
}

// -------------------------
// 线程池类
// -------------------------
class ThreadPool {
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running;

    // 统计信息
    vector<int> threadTaskCount;
    atomic<int> totalExceptionCount;

public:
    ThreadPool(size_t threadNum)
        : running(true), threadTaskCount(threadNum, 0), totalExceptionCount(0)
    {
        for (size_t i = 0; i < threadNum; ++i) {
            workers.emplace_back([this, i]() {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        condvar.wait(lock, [this]() { return !tasks.empty() || !running; });
                        if (!running && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop();
                    }

                    auto start = high_resolution_clock::now();
                    try {
                        task();
                    }
                    catch (const exception& e) {
                        string msg = "[" + now_datetime() + "][Thread " + to_string(i) + "] exception: " + e.what();
                        log_line(msg);
                        totalExceptionCount++;
                    }
                    catch (...) {
                        string msg = "[" + now_datetime() + "][Thread " + to_string(i) + "] unknown exception.";
                        log_line(msg);
                        totalExceptionCount++;
                    }
                    auto end = high_resolution_clock::now();
                    auto duration = duration_cast<milliseconds>(end - start).count();

                    threadTaskCount[i]++;
                    string msg = "[Thread " + to_string(i) + "] task completed, duration: " + to_string(duration) + " ms, total tasks executed: " + to_string(threadTaskCount[i]);
                    log_line(msg);
                }
            });
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    void submit(function<void()> task) {
        if (!running) throw runtime_error("ThreadPool is stopped, cannot submit task.");
        {
            lock_guard<mutex> lock(queueMutex);
            tasks.push(move(task));
        }
        condvar.notify_one();
    }

    void shutdown() {
        running = false;
        condvar.notify_all();
        for (auto& t : workers) if (t.joinable()) t.join();

        log_line("\n=== ThreadPool Shutdown Complete ===");
        log_line("Total exceptions: " + to_string(totalExceptionCount.load()));
        for (size_t i = 0; i < threadTaskCount.size(); ++i) {
            log_line("Thread " + to_string(i) + " executed " + to_string(threadTaskCount[i]) + " tasks");
        }
    }
};

// -------------------------
// 主函数
// -------------------------
int main() {
    ThreadPool pool(4);

    // 1️⃣ 普通任务（打印开始和结束）
    for (int i = 1; i <= 5; ++i) {
        pool.submit([i]() {
            ostringstream oss;
            oss << this_thread::get_id();
            string tid_str = oss.str();

            string msg = "Task " + to_string(i) + " started, thread id: " + tid_str;
            log_line(msg);

            this_thread::sleep_for(chrono::milliseconds(500));

            msg = "Task " + to_string(i) + " end";
            log_line(msg);
        });
    }

    // 2️⃣ 异常任务
    pool.submit([]() { throw runtime_error("simulated exception"); });
    pool.submit([]() { throw 123; });

    // 3️⃣ 长任务（模拟耗时 3 秒）
    pool.submit([]() {
        ostringstream oss;
        oss << this_thread::get_id();
        string tid_str = oss.str();

        string msg = "Long task started, thread id: " + tid_str;
        log_line(msg);

        this_thread::sleep_for(chrono::seconds(3));

        msg = "Long task end";
        log_line(msg);
    });

    // 4️⃣ 后续普通任务（异常后继续提交）
    for (int i = 6; i <= 8; ++i) {
        pool.submit([i]() {
            ostringstream oss;
            oss << this_thread::get_id();
            string tid_str = oss.str();

            string msg = "Task " + to_string(i) + " started after exceptions, thread id: " + tid_str;
            log_line(msg);

            this_thread::sleep_for(chrono::milliseconds(500));

            msg = "Task " + to_string(i) + " end";
            log_line(msg);
        });
    }

    // 等待所有任务完成
    this_thread::sleep_for(chrono::seconds(8));
    pool.shutdown();

    return 0;
}
