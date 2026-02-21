性能量化：
使用top -H -p tid可以知道这个程序的各个线程使用CPU和内存情况
使用perf record -g -p tid   perf report可以得到详细 CPU 分析，查看那个函数占用时间最多

valgrind --leak-check=full ./server：内存泄漏检测：(这种测试的执行方法对tps影响很大，可以先执行程序，单独开一个终端执行，这样影响会小一点)
(base) wyh@wyh-VirtualBox:/media/sf_System-C/Edge 3/output$ valgrind ./d1_server
==4232== Memcheck, a memory error detector
==4232== Copyright (C) 2002-2022, and GNU GPL'd, by Julian Seward et al.
==4232== Using Valgrind-3.22.0 and LibVEX; rerun with -h for copyright info
==4232== Command: ./d1_server
==4232== 
[Server] listening on port 8080
[Server] stopped.
==4232== 
==4232== HEAP SUMMARY:
==4232==     in use at exit: 0 bytes in 0 blocks
==4232==   total heap usage: 106,473 allocs, 106,473 frees, 5,703,272 bytes allocated
==4232== 
==4232== All heap blocks were freed -- no leaks are possible
==4232== 
==4232== For lists of detected and suppressed errors, rerun with: -s
==4232== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

strace -c ./server：系统调用分析(这种测试的执行方法对tps影响很大，可以先执行程序，单独开一个终端执行，这样影响会小一点)
(base) wyh@wyh-VirtualBox:/media/sf_System-C/Edge 3/output$ strace -c ./d1_server
[Server] listening on port 8080
strace: Process 4798 detached
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 31.61    3.829175          54     70664     35280 read
 29.78    3.607190         101     35569        57 futex
 25.10    3.039973          86     35281           write
 12.94    1.567340          44     35481           epoll_ctl
  0.49    0.059867          83       716         1 epoll_wait
  0.07    0.007963          75       105           close
  0.01    0.000942           9       101         1 accept
  0.01    0.000652           3       202           fcntl
  0.00    0.000000           0         6           fstat
  0.00    0.000000           0        27           mmap
  0.00    0.000000           0        11           mprotect
  0.00    0.000000           0         1           munmap
  0.00    0.000000           0         3           brk
  0.00    0.000000           0         3           rt_sigaction
  0.00    0.000000           0        11           rt_sigprocmask
  0.00    0.000000           0         2           pread64
  0.00    0.000000           0         1         1 access
  0.00    0.000000           0         1           socket
  0.00    0.000000           0         1           bind
  0.00    0.000000           0         1           listen
  0.00    0.000000           0         1           setsockopt
  0.00    0.000000           0         1           execve
  0.00    0.000000           0         1           arch_prctl
  0.00    0.000000           0         1           set_tid_address
  0.00    0.000000           0         5           openat
  0.00    0.000000           0         1           set_robust_list
  0.00    0.000000           0         1           epoll_create1
  0.00    0.000000           0         1           prlimit64
  0.00    0.000000           0         1           getrandom
  0.00    0.000000           0         1           rseq
  0.00    0.000000           0         5           clone3
------ ----------- ----------- --------- --------- ----------------
100.00   12.113102          67    178206     35340 total
[Server] shutting down...



内存管理优化：
智能指针：
unique_ptr，本质：独占所有权指针，一个资源只能有一个拥有者。
不能拷贝，可以移动，析构自动释放，零额外开销（几乎等同裸指针）。
适用场景：文件句柄，socket，线程对象，独占资源，Service 内部成员90% 情况应该优先用 unique_ptr。
他不需要delete，即使发生了异常，也会释放资源。因为它实际上是一个类对象，发生了异常之后，会进行栈展开，在这个过程中，类对象会被析构。而普通的裸指针，没有办法。

shared_ptr，本质：共享所有权指针，多个对象共同拥有一个资源。内部机制：引用计数，最后一个释放时才 delete
可以拷贝，自动引用计数，线程安全计数（原子操作），有一定性能开销
适用场景：复杂对象图，观察者模式，任务系统共享对象，生命周期难以单点管理时

weak_ptr，本质：不拥有资源的“观察者”，用于解决 shared_ptr 的循环引用问题。
weak_ptr 是一个“不会增加引用计数的观察者”。他不用有资源，但是他知道资源的使用情况。
他不能被直接使用，也就是*wp，它的用法是p=wp.lock()
lock() 会：检查引用计数是否 > 0。如果活着 → 返回 shared_ptr；如果死了 → 返回空 shared_ptr。
裸指针不知道对象是否已经被销毁,而weak_ptr可以安全检查。所以不能使用裸指针代替weak_ptr
示例
shared_ptr<A> a = make_shared<A>();
shared_ptr<B> b = make_shared<B>();
a->b = b;
b->a = a;  // 循环引用
此时引用计数永远不为 0 → 内存泄漏。
解决方式：
weak_ptr<A> a;
weak_ptr 不增加引用计数。

Deleter 本质上就是一个在对象生命周期结束时调用的自定义释放逻辑。
任何需要 RAII 管理资源的对象都可以有类似 deleter 的机制。

C++ 异常安全有三个等级：
等级	含义
基本保证	不泄漏资源
强保证	状态一致
无异常保证	不抛异常

修改的代码意义以及不修改的潜在问题是：（但是一个用了unique_ptr，一个没有使用，因为一个是指针，一个是整数）
1、从“手动管理裸指针”变成“语言级 RAII 管理”。
可以明确资源所有权，比如如果有人写了File* temp=fptr（当然在我原来的函数中，也不会运行这样拷贝，但是如果别人是在类中引入了temp，而且用的是move，那么可能还是会导致问题），那么可能会导致资源所有权混乱
避免资源无法释放和内存泄漏

2、（最重要的修改）修改了fd的方式。
如果原来的代码在创建fd之后、释放fd之前就出现了问题，那么最终会导致分配的资源也会无法释放，并且在高并发的情况下可能会耗尽fd资源



CPU优化：
第一次优化：
客观的讲，优化代码相比于原始代码而言在 条件：4个线程的线程池，100个客户端，1000条信息，间隔1ms  、 条件：8个线程的线程池，500客户端，1000条信息，间隔1ms   条件：8个线程的线程池，200客户端，1000条信息，5ms间隔 的情况下都没有表现出很好的TPS优化效果

在8个线程的线程池，500客户端，1000条信息，间隔2ms的情况下，优化后的效果更加稳定，效果确实更好一些

这里的优化（指的是处理读和写任务）意思是说将涉及到任务的数据提前拷贝下来，这样避免多个线程同时竞争这个数据的时候需要利用锁来维护

使用sudo perf top -p pid，可以知道第一版的优化代码大量时间都在上下文切换上
优化方向就非常清晰了：
❌ 减少线程池共享队列
❌ 减少 condvar 唤醒
❌ 减少锁竞争
✅ 让连接“固定归属某个线程”
✅ 让线程不需要抢锁

第二次优化：
使用多个epoll，每个线程有一个epoll，使得TPS速度变大很多，优化效果非常明显。

旧架构：1个 epoll 线程 ---> 共享任务队列 --->线程池抢任务
特点：多线程竞争任务，有锁，有上下文切换，有任务迁移，有 cache 失效

新架构：主线程 accept ---> 轮询分配连接 ---> 每个 worker 自己 epoll
特点：每个线程一个 epoll,每个连接只属于一个线程,无共享队列,无锁,无线程抢占

为什么性能能提升那么多？主要是解决了三个问题
1、锁竞争
在旧版本的时候，在处理任务、选择线程的时候，就会存在锁的加锁和解锁问题；新版本直接不存在锁，所以开销好了很多。
2、上下文切换
没有线程间抢任务，上下文切换的开销小了很多
3、cache失效
旧版本中，线程之间会存在抢任务的情况，那么就会存在原来这个任务在线程1执行，后来去了线程2，那么CPU中cache关于这个任务的信息就会被浪费，即cache失效。新版本是利用了  CPU Cache 亲和性 。这是高性能网络服务器的核心。



高并发优化：
这里有两种架构：单epoll+线程池+batch  和  每个线程对应一个epoll  他们的TPS效果其实差不多。
🔵 单 epoll + 线程池 + batch
优点：结构简单，容易扩展阻塞型业务，线程可以弹性处理负载，容易实现优先级队列，更灵活（任务模型）
缺点：存在锁竞争，线程间 cache 抖动，context switch 更多，扩展到极高并发时调度成本高

🟢 每线程一个 epoll
优点：无共享锁，fd 不跨线程，cache 友好，调度次数更少，更接近工业级高性能模型
缺点：负载不均衡时难调节，不适合阻塞任务，扩展业务复杂度高，需要设计连接分配策略

线程池模型是“任务驱动”，per-thread epoll 是“事件驱动”