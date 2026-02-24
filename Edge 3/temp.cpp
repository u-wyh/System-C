#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <chrono>

int main() {
    const std::string filename = "test_switch.txt";
    const int TOTAL_LINES = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    // ==============================
    // 方式 A：正常 C++ 缓冲写入
    // ==============================
    {
        std::ofstream fout(filename, std::ios::out | std::ios::trunc);
        if (!fout.is_open()) {
            std::cerr << "Failed to open file (C++ mode)\n";
            return 1;
        }

        for (int i = 0; i < TOTAL_LINES; ++i) {
            fout << "This is line " << i 
                 << " for testing file IO performance.\n";
        }

        fout.close();
    }

    // ==============================
    // 方式 B：暴力 write（每行一个 syscall）
    // ==============================
    {
        int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open failed");
            return 1;
        }

        for (int i = 0; i < TOTAL_LINES; ++i) {
            std::string line = "This is line " + 
                               std::to_string(i) + 
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

    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "Write completed in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
              << " ms\n";

    return 0;
}