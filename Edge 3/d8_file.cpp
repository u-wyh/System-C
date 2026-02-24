#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

using namespace std;

int main() {
    std::cout.sync_with_stdio(0);
    const std::string filename = "test.txt";

    // Step 1: 写文件
    {
        auto start = std::chrono::high_resolution_clock::now();

        std::ofstream fout(filename, std::ios::out | std::ios::trunc);
        if (!fout.is_open()) {
            std::cerr << "Failed to open " << filename << " for writing.\n";
            return 1;
        }

        // 写入 10000 行测试数据
        for (int i = 0; i < 100; ++i) {
            fout << "This is line " << i << " for testing file IO performance.\n";
        }

        fout.close();

        auto end = std::chrono::high_resolution_clock::now();
        std::cout << "Write completed in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
                  << " ms\n";
    }
    
    {
        int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open failed");
            return 1;
        }

        for (int i = 0; i < 100; ++i) {
            std::string line = "This is line " + 
                               std::to_string(i+10000) + 
                               " for brutal syscall testing.\n";

            ssize_t ret = write(fd, line.c_str(), line.size());
            if (ret < 0) {
                perror("write failed");
                close(fd);
                return 1;
            }
        }

        close(fd);
    }

    // Step 2: 读文件
    {
        auto start = std::chrono::high_resolution_clock::now();

        std::ifstream fin(filename, std::ios::in);
        if (!fin.is_open()) {
            std::cerr << "Failed to open " << filename << " for reading.\n";
            return 1;
        }

        std::string line;
        int count = 0;
        while (std::getline(fin, line)) {
            // 可以选择打印前 10 行测试数据
            if (count < 10) std::cout << line << std::endl;
            ++count;
        }

        fin.close();

        auto end = std::chrono::high_resolution_clock::now();
        std::cout << "Read completed in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
                  << " ms\n";
        std::cout << "Total lines read: " << count << "\n";
    }

    // Step 3: 模拟阻塞情况
    {
        std::cout << "\nSimulating blocking read (sleep 3s)...\n";
        std::ifstream fin(filename, std::ios::in);
        std::string line;
        if (std::getline(fin, line)) {
            std::cout << "First line: " << line << std::endl;
        }
        std::cout << "Now sleep to simulate blocking IO...\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout << "Finished sleep, continue reading...\n";
        fin.close();
    }

    return 0;
}
// (base) wyh@wyh-VirtualBox:~$ cd "/media/sf_System-C/Edge 3"
// (base) wyh@wyh-VirtualBox:/media/sf_System-C/Edge 3$ strace -c ./d8_file


// 第一次的结果
// % time     seconds  usecs/call     calls    errors syscall
// ------ ----------- ----------- --------- --------- ----------------
//  51.24    0.026780         439        61           writev
//  31.12    0.016264         235        69           read
//   4.88    0.002548         141        18           write
//   3.81    0.001990          90        22           mmap
//   1.92    0.001006        1006         1           execve
//   1.64    0.000857         107         8           close
//   1.62    0.000849         106         8           openat
//   0.73    0.000382          63         6           fstat
//   0.69    0.000363          60         6           mprotect
//   0.42    0.000221         221         1           clock_nanosleep
//   0.38    0.000198          66         3           brk
//   0.26    0.000138          69         2           pread64
//   0.24    0.000123         123         1           set_tid_address
//   0.16    0.000085          85         1           munmap
//   0.15    0.000076          76         1         1 access
//   0.13    0.000068          68         1           futex
//   0.13    0.000068          68         1           prlimit64
//   0.13    0.000067          67         1           set_robust_list
//   0.13    0.000066          66         1           arch_prctl
//   0.13    0.000066          66         1           rseq
//   0.09    0.000047          47         1           getrandom
// ------ ----------- ----------- --------- --------- ----------------
// 100.00    0.052262         244       214         1 total

// 第四次的结果：  体现了cache的效果
// % time     seconds  usecs/call     calls    errors syscall
// ------ ----------- ----------- --------- --------- ----------------
//  44.01    0.017690         290        61           writev
//  35.23    0.014161         205        69           read
//   5.80    0.002331         129        18           write
//   5.40    0.002169          98        22           mmap
//   2.57    0.001034         129         8           openat
//   2.24    0.000902         112         8           close
//   1.27    0.000510          85         6           mprotect
//   1.10    0.000443          73         6           fstat
//   0.58    0.000232         232         1           clock_nanosleep
//   0.26    0.000106         106         1           munmap
//   0.26    0.000103          34         3           brk
//   0.25    0.000101          50         2           pread64
//   0.18    0.000072          72         1           set_robust_list
//   0.17    0.000069          69         1           arch_prctl
//   0.17    0.000069          69         1           futex
//   0.16    0.000066          66         1           rseq
//   0.12    0.000049          49         1           getrandom
//   0.11    0.000046          46         1           set_tid_address
//   0.11    0.000045          45         1           prlimit64
//   0.00    0.000000           0         1         1 access
//   0.00    0.000000           0         1           execve
// ------ ----------- ----------- --------- --------- ----------------
// 100.00    0.040198         187       214         1 total

// 频繁使用系统调用的结果
// % time     seconds  usecs/call     calls    errors syscall
// ------ ----------- ----------- --------- --------- ----------------
//  98.28    2.631173         262     10013           write
//   1.02    0.027226         446        61           writev
//   0.53    0.014239         222        64           read
//   0.07    0.001904         211         9           openat
//   0.04    0.001090         121         9           close
//   0.03    0.000788         788         1           execve
//   0.01    0.000396          18        22           mmap
//   0.01    0.000227         227         1           clock_nanosleep
//   0.00    0.000069          13         5           fstat
//   0.00    0.000060          15         4           brk
//   0.00    0.000058          58         1         1 access
//   0.00    0.000057           9         6           mprotect
//   0.00    0.000016          16         1           munmap
//   0.00    0.000006           3         2           pread64
//   0.00    0.000004           4         1           arch_prctl
//   0.00    0.000004           4         1           futex
//   0.00    0.000004           4         1           getrandom
//   0.00    0.000003           3         1           set_robust_list
//   0.00    0.000003           3         1           prlimit64
//   0.00    0.000002           2         1           set_tid_address
//   0.00    0.000002           2         1           rseq
// ------ ----------- ----------- --------- --------- ----------------
// 100.00    2.677331         262     10206         1 total
