#include <iostream>
#include <thread>
#include <chrono>

int main() {
    // cout 不换行，也不 flush
    std::cout << "This is cout...";

    // cerr 直接输出
    std::cerr << "This is cerr!\n";

    // 等一秒再输出 cout 的刷新内容
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Now cout appears!\n";

    return 0;
}
