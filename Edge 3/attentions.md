性能量化：
使用top -H -p tid可以知道这个程序的各个线程使用CPU和内存情况
使用perf record -g -p tid   perf report可以得到详细 CPU 分析，查看那个函数占用时间最多
valgrind --leak-check=full ./server：内存泄漏检测
strace -p tid：系统调用分析