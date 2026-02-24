// file_io_demo.cpp
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

int main() {
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
        for (int i = 0; i < 10000; ++i) {
            fout << "This is line " << i << " for testing file IO performance.\n";
        }

        fout.close();

        auto end = std::chrono::high_resolution_clock::now();
        std::cout << "Write completed in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
                  << " ms\n";
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