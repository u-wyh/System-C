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



内核系统：
文件和 Socket 的内核操作
文件操作：open → read → write → close
Socket 操作：socket → bind → listen → accept → recv/send → close
重点是理解这些调用背后内核做了什么（页缓存、文件描述符分配、网络缓冲、阻塞等待等）

| 用户态操作   | 系统调用                      | 内核数据结构                  | 内核行为                        |
| -------     | ------------------------- | ----------------------- | --------------------------- |
| 打开文件    | open/openat               | file struct, inode      | 分配 fd，建立内核文件结构              |
| 写文件     | write/writev              | file struct, page cache | 拷贝数据 → 内核缓冲区 → 标记 dirty     |
| 读文件     | read/pread                | file struct, page cache | 从 page cache 或磁盘拷贝到用户空间     |
| 关闭文件    | close                     | file struct             | flush dirty page（必要时），释放 fd |
| 睡眠/模拟阻塞 | nanosleep/clock_nanosleep | task_struct             | 挂起线程 → CPU 调度其他线程           |
| 输出到终端   | write(1)                  | tty driver, buffer      | 拷贝到终端缓冲 → 写屏 / 阻塞           |

文件 IO 性能观察

缓冲机制：
用户态缓冲：如 std::ofstream，写入时先放在程序缓冲区，减少系统调用次数。
内核态缓冲：page cache，写入磁盘前先存内核缓冲，异步刷新。

实验结论：
暴力写（每行直接 write()） → 大量系统调用 → CPU 占用高、吞吐量低。
缓冲写（ofstream 或 writev） → 减少系统调用次数 → 吞吐量高。
阻塞不会占用 CPU，但会延迟线程完成 → 单线程中效率低，多线程会产生上下文切换开销。

吞吐量：单位时间内完成的操作数量（如 MB/s 或 行数/s）。
延迟：单次操作从发起到完成的时间。

| 用户态操作        | 系统调用                                 | 内核数据结构                              | 内核行为                                        |
| ------------ | ------------------------------------ | ----------------------------------- | ------------------------------------------- |
| 创建套接字        | `socket(AF_INET, SOCK_STREAM)`       | `struct socket`, `struct inet_sock` | 分配 socket，初始化 TCP 控制块，注册内核资源                |
| 绑定端口         | `bind(sockfd, addr, len)`            | `struct socket`, `inet_sock`        | 检查端口是否可用 → 绑定 socket 和 IP+端口 → 更新内核端口分配表    |
| 监听端口         | `listen(sockfd, backlog)`            | `struct socket`, `listen_queue`     | 创建半连接队列 + 完全连接队列，等待三次握手                     |
| 接受客户端连接（阻塞）  | `accept(sockfd)`                     | `struct socket`, `inet_sock`        | 阻塞等待连接到来 → 三次握手完成 → 分配新的 socket fd          |
| 接受客户端连接（非阻塞） | `accept(sockfd, O_NONBLOCK)`         | `struct socket`, `inet_sock`        | 检查队列是否有连接 → 若无立即返回 `-1/EAGAIN`              |
| 接收数据（阻塞）     | `recvfrom(fd, buf, len, 0)`          | `socket buffer`, `task_struct`      | 若缓冲区无数据 → 进程挂起 → 数据到达时唤醒 → 拷贝到用户空间          |
| 接收数据（非阻塞）    | `recvfrom(fd, buf, len, O_NONBLOCK)` | `socket buffer`                     | 检查缓冲区 → 若无数据立即返回 `-1/EAGAIN`                |
| 发送数据         | `write(fd, buf, len)`                | `socket buffer`, `tcp_skb`          | 拷贝用户数据到内核缓冲区 → TCP 封包发送 → 若缓冲区满阻塞/返回 EAGAIN |
| 关闭连接         | `close(fd)`                          | `socket buffer`, `struct socket`    | 四次挥手关闭 TCP → 释放 PCB、缓冲区 → 解除端口绑定            |
| 模拟非阻塞等待（轮询）  | `nanosleep` / `clock_nanosleep`      | `task_struct`                       | 挂起线程 → CPU 调度其他线程，减少轮询 CPU 占用               |


线程迁移（Thread Migration）：
内核调度器将线程从一个 CPU 核心迁移到另一个核心
目的：负载均衡，充分利用多核 CPU
代价：缓存失效（L1/L2）、TLB 失效 → 性能下降

CPU 亲和性（Thread Affinity / CPU Binding）：
固定线程在指定核心上执行
优势：减少线程迁移，提高 L1/L2 缓存命中率，延迟更稳定
劣势：线程数大于核心数时，可能增加上下文切换 → TPS 下降

上下文切换（Context Switch）：
CPU 切换执行线程时保存/恢复寄存器状态和栈信息
开销包括：寄存器保存、TLB / cache 失效、调度器计算
线程过多 → 上下文切换次数增多 → TPS 下降

Linux 调度器（CFS）特性：
调度器可动态迁移线程到负载较低的核心
小线程数时，CFS 已经能做到较合理的负载均衡
大线程数 + 核心少 → 线程绑定有利于延迟稳定



内核网络栈：
(base) wyh@wyh-VirtualBox:~$ ss -s
Total: 847
TCP:   5 (estab 0, closed 0, orphaned 0, timewait 0)

Transport Total     IP        IPv6
RAW	  0         0         0        
UDP	  7         5         2        
TCP	  5         4         1        
INET	  12        9         3        
FRAG	  0         0         0 



false sharing实验：
False Sharing 是指多个线程修改不同变量，但这些变量位于同一个 cache line 中，导致缓存一致性协议频繁失效，从而造成性能下降的现象。



容器化服务：
Docker 是一个：基于 Linux 容器技术的应用打包、分发、运行平台
一句更工程化的话：Docker 是一个“应用运行时 + 镜像构建系统 + 分发系统”的组合体。

Docker 出现前的问题：开发环境 OK；测试环境 OK；线上环境 崩。这叫：环境不可移植
Docker 解决了三件事：
1、应用 + 依赖打包成一个整体。全部打包成一个镜像（Image），这样：到哪里运行都一样
2、运行时隔离。通过 Linux 内核技术，实现：进程隔离，网络隔离，文件系统隔离，CPU / 内存限制。  但注意：⚠ 容器不是虚拟机，它共享宿主机内核
3、标准化分发。Docker 设计了：镜像格式标准，Registry 仓库标准，拉取 / 推送标准


Docker 架构（非常重要）
Docker 本质是 C/S 架构：docker CLI  →  dockerd  →  containerd  →  runc

在 Docker 里：镜像 = 一个“准备好的运行环境包”
镜像 ≈ 一个已经准备好的运行环境,更准确一点是：镜像 = 一个“静态的环境模板”
它是：打包好的,还没运行,放在磁盘上的;  就像：安装包还没双击,游戏还没打开
等你执行：docker run xxx,它才变成：一个真正“运行起来的环境”
执行了docker run xxx之后，“根据镜像创建了一个容器”，镜像本身不会消失，也不会变。
镜像 = 蛋糕的模具，容器 = 用模具做出来的蛋糕

“镜像”这个词在计算机里本来就有两个常见含义：
1️⃣ 服务器副本（apt 镜像站）
2️⃣ 磁盘/系统完整拷贝（Docker 镜像）
本质都是：一份完整复制，只是使用场景不同。
apt 镜像源 = 软件仓库的服务器副本
Docker 镜像 = 一个打包好的运行环境


镜像（Image）：静态的、不可变的文件系统 + 应用环境。就像一张光盘。
容器（Container）：镜像的运行实例。就像你把光盘放进电脑启动，它变成了一个活生生的系统，可以操作、运行程序、产生数据。

容器的特点
轻量级：它不需要完整虚拟机，直接共享宿主机内核。
隔离性：容器内部的操作不会影响宿主机和其他容器。
可短暂存在：容器可以随时删除，重新启动新的容器又是干净环境。
可重复部署：同一个镜像可以启动成无数个容器，每个都是一致的环境。

在感受上：容器 ≈ 轻量虚拟机。这样理解没问题。
但本质区别在哪里？（只讲一个点）
虚拟机（比如 VirtualBox）：模拟了一整套硬件，有自己的内核，完整操作系统
容器（在 Docker 里）：不模拟硬件，不包含内核，共享宿主机内核，本质只是被隔离的进程

容器之所以轻量级，核心原因就是 容器共享宿主机的内核，不需要自己带一整个操作系统内核。（但这也要求linux docker需要部署到linux系统上，不然无法共用内核）

注册中心（Registry）：用来存放镜像的地方
官方的 Docker Hub 是最常用的，但国内也有阿里云镜像、网易云镜像等
你 pull 镜像就是从注册中心下载模板，你 push 镜像就是把你改好的镜像上传到注册中心

Docker 的局限
你既然学到现在这个阶段，我必须讲缺点：
隔离不如虚拟机，依赖 Linux 内核，单机管理能力有限，生产环境需要编排系统
这就引出了：👉 Kubernetes


Docker 的核心组件：
Docker Engine（引擎）：
  核心运行时，负责创建、运行、停止容器。
  分两部分：
    dockerd（Docker Daemon）：后台服务，管理镜像、容器、网络、存储。
    docker CLI：命令行工具，你输入 docker run、docker build 等命令就是在和 Daemon 交流。
镜像（Image）
容器（Container）
Docker Registry（仓库/注册中心）

CLI 发命令 → Daemon 执行 → 创建容器/管理镜像 → 可以从 Registry 下载或上传镜像
简单来说，Docker CLI 只是你和 Docker 的沟通工具，Docker Daemon 才是真正干活的，镜像是模板，容器是实例，Registry 是存放镜像的地方。

拆解 namespace 和 cgroup，这是理解容器隔离和资源控制的核心机制。

Namespace（命名空间）

概念：Linux 提供的一种机制，用于隔离系统资源，让不同进程“看到”的环境不同。容器就是通过多种 namespace 组合，把一个进程隔离成一个“自己的小世界”。

常用的 namespace 类型：
| 类型       | 隔离内容   | 举例                              |
| -------- | ------ | ------------------------------- |
| **pid**  | 进程号    | 容器里的进程号从 1 开始，不影响宿主机            |
| **net**  | 网络     | 容器可以有自己的网卡、IP、端口映射              |
| **mnt**  | 文件系统挂载 | 容器可以挂载自己的文件系统，不看到宿主机的部分目录       |
| **uts**  | 主机名/域名 | 容器可以有自己的主机名                     |
| **ipc**  | 进程间通信  | 容器内部的进程用共享内存、信号量通信，不影响宿主机       |
| **user** | 用户与权限  | 容器内部可以有自己的用户ID映射，不直接使用宿主机的 root |

1️⃣ C++ 中的 namespace
作用：避免 名字冲突。
举例：你有两个不同库都定义了一个 print() 函数，你可以用 A::print() 和 B::print() 来区分。
本质：纯粹是编译器层面的名字管理，不影响程序运行时的行为，也不隔离资源或进程。

2️⃣ Linux 容器中的 namespace
作用：隔离 系统资源，让进程感觉自己在独立系统里运行。
举例：容器里可以有自己的 PID、网络接口、挂载点。即便 PID 是 1，也不会影响宿主机上的进程。
本质：内核级别的资源隔离，影响程序运行时的行为和可见系统资源。


cgroup = Control Group（控制组）
它的功能是：限制、计量和管理一组进程使用的物理资源

具体资源包括：
CPU（CPU 时间配额）
内存（内存上限）
IO（磁盘读写速率）
网络带宽（网络流量）

换句话说：
namespace 让容器“看不到别人”，cgroup 让容器“不能随便吃资源”

我们必须要先有一个基础的镜像，在这个基础的镜像之上添加我们的功能，形成新的镜像并运行，容器内可以按照我们写的dockerfile来进行工作
docker run --rm -p 8080:8080 d13_log_image  这个操作的意思是将容器内的8080端口设置为宿主机的8080端口用

基础镜像
Docker 镜像本质上是一个操作系统 + 环境的快照
你必须有一个基础镜像（比如 Ubuntu），作为起点
在这个基础镜像上安装依赖、复制代码、编译程序 → 形成新的镜像（你的是 d13_log_image）

Dockerfile 的作用
Dockerfile 就是“建造说明书”
容器会按照 Dockerfile 的指令去做：
安装软件、拷贝代码、编译程序、设置启动命令（CMD）、运行容器并映射端口

docker run --rm -p 8080:8080 d13_log_image
--rm → 容器停止后自动删除
-p 8080:8080 → 把容器内部的 8080 端口映射到宿主机的 8080 端口
容器内部程序监听 8080
宿主机通过 8080 就能访问容器服务
d13_log_image → 要运行的镜像

运行效果
容器启动后，服务程序在容器内部运行
你在宿主机访问 localhost:8080 就相当于访问容器内部服务

使用docker images可以查看有了几个images  以及各个image的大致情况
使用docker rmi +“image id” 可以删除指定编号的镜像


作用：选择一个已有的 Ubuntu 镜像作为基础
FROM 表示继承这个镜像，所有基础的 Linux 系统工具都会有
FROM docker.xuanyuan.run/library/ubuntu:latest

把你本地的 d13_log.cpp 文件拷贝到容器内部的 /app/ 目录
COPY d13_log.cpp /app/
设置 当前工作目录，后续的命令（比如编译、运行）都会在 /app 下执行
WORKDIR /app

RUN 表示在镜像构建时执行命令
这里更新了 apt 软件源，并安装了 g++ 和 make，保证容器中有 C++ 编译环境
-y 表示自动确认安装
RUN apt update && apt install -y g++ make

编译你的 C++ 源代码 d13_log.cpp
输出可执行文件 d13_log
编译过程在构建镜像时完成，所以容器启动时已经有编译好的程序
RUN g++ d13_log.cpp -o d13_log

CMD 指定容器启动后默认执行的命令
当你运行容器时，会自动执行 ./d13_log
注意：CMD 只会在容器启动时运行，不会在构建镜像时运行
CMD ["./d13_log"]



弹性扩容：
Nginx：
Nginx 是一个：🔥 高性能 Web 服务器 + 反向代理服务器 + 负载均衡器
最早它是为了解决：C10K 问题（同时处理 1 万个并发连接）
传统服务器模型是：一个连接 → 一个线程。线程多了就崩。
Nginx 的创新是：事件驱动 + 非阻塞 IO。这和你现在写的 epoll 服务器本质思想一样。

Nginx 有 4 大核心能力：
1️⃣ Web 服务器 很多网站的静态资源就是 Nginx 直接提供的。
2️⃣ 反向代理 客户端根本不知道后面有多少机器。
3️⃣ 负载均衡
4️⃣ 限流与安全控制：限制 QPS、限制 IP 访问频率、做黑名单、SSL 终止（HTTPS解密）

HAProxy：
HAProxy 是一个：🔥 高性能 TCP/HTTP 负载均衡器 + 反向代理软件
特点：
专注 负载均衡和高可用、
用 C 语言实现，极度轻量、高性能、
广泛用于金融、电商、大型网站等对性能和稳定性要求极高的场景

HAProxy可以干什么：
1️⃣ 负载均衡：支持多种策略（轮询 最少连接 源IP 基于权重）
2️⃣ 反向代理
3️⃣ 高可用
4️⃣ 安全控制

| 特性    | Nginx            | HAProxy               |
| ----- | ---------------- | --------------------- |
| 核心用途  | Web 服务器 + 反向代理   | 负载均衡 + 反向代理           |
| 层级    | HTTP/HTTPS 层（七层） | TCP 层（四层）+ HTTP 层（七层） |
| 静态资源  | 可以直接提供           | 不提供，专注转发              |
| 高并发   | 高性能事件驱动          | 极致性能、低延迟              |
| 配置复杂度 | 易读，丰富功能          | 简洁、专注负载均衡和健康检查        |
| 模块扩展  | 丰富模块             | 少量扩展，稳定优先             |

简单来说：
Nginx = 工业级 Web 服务器 + 通用反向代理
HAProxy = 极致性能的负载均衡器（TCP/HTTP）

用 Dockerfile 构建 epoll_server 镜像，启动三个容器：
docker run -d --name server1 -p 8081:8080 epoll_server:latest
docker run -d --name server2 -p 8082:8080 epoll_server:latest
docker run -d --name server3 -p 8083:8080 epoll_server:latest

使用 Nginx 配置轮询：
upstream backend {
    server 127.0.0.1:8081;
    server 127.0.0.1:8082;
    server 127.0.0.1:8083;
}

server {
    listen 8000;
    location / {
        proxy_pass http://backend;
    }
}

启动 Nginx 容器，挂载配置文件：
docker run -d --name nginx_lb -p 8000:8000 -v /path/load_balancer.conf:/etc/nginx/conf.d/load_balancer.conf:ro docker.xuanyuan.run/library/nginx:latest


弹性扩容（Elastic Scaling / Horizontal Scaling）核心概念：
什么是弹性扩容
服务可以 动态增加或减少后端服务器实例，以应对不同的负载压力
“弹性”强调 按需伸缩：负载高时增加服务器，低时减少服务器
目标是 保证服务性能、响应时间和稳定性

横向扩展 vs 纵向扩展
横向扩展（Horizontal Scaling）：增加服务器节点（你这次实验的方式）
优点：TPS 总量累加，单机压力不大
缺点：需要负载均衡和分布式管理
纵向扩展（Vertical Scaling）：增加单台服务器性能（CPU/内存）
优点：部署简单
缺点：性能有限，成本高

实验中的体现
你启动了 server1 / server2 / server3 三台容器，Nginx 做负载均衡，客户端压力增大 → 三台服务器共同分担请求，TPS 总量提升 → 就体现了横向弹性扩容的效果

实际意义
当用户访问量突然增加时，你可以 快速增加服务器实例
流量均匀分发 → 保证响应时间稳定
成本和资源利用更灵活

💡 总结一句话：弹性扩容就是通过增加后台服务器实例，实现更高吞吐量、更稳定服务，Nginx/HAProxy 等负载均衡就是弹性扩容的关键手段之一。



配置与热更新：
热更新 = 在服务运行过程中，不停止进程、不重启服务，直接修改其行为或配置。