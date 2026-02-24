#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <chrono>
#include <fcntl.h>
#include <error.h>
#include <unistd.h>
#include <cstring>

using namespace std;

const int PORT = 8080;
const int BUFFER_SIZE = 8192;

int main() {
    // 1. 创建 TCP 套接字
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    // 2. 绑定地址
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    // 3. 监听
    if (listen(listen_fd, 10) < 0) { perror("listen"); return 1; }
    cout << "[Server] Listening on port " << PORT << "...\n";

    // 4. 接受连接
    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) { perror("accept"); return 1; }
    cout << "[Server] Client connected.\n";

    // 设置为非阻塞式
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // 5. 循环接收数据
    char buffer[BUFFER_SIZE];
    int total_bytes = 0;
    auto start = chrono::high_resolution_clock::now();

    while (true) {
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            // 客户端关闭连接
            break;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据，非阻塞返回 -1，这里可以做其他事情
                // 例如短暂 sleep 或记录时间
                usleep(1000000); // 避免空循环占用 CPU
                // cout<<"no information"<<endl;
                continue;
            } else {
                cerr << "recv error: " << strerror(errno) << endl;
                break;
            }
        }
        total_bytes += n;
        // 可以选择打印每条消息，也可以注释掉避免影响性能
        cout.write(buffer, n);
    }

    auto end = chrono::high_resolution_clock::now();
    cout << "[Server] Total bytes received: " << total_bytes
         << ", time: " << chrono::duration_cast<chrono::milliseconds>(end - start).count()
         << " ms\n";

    // 6. 关闭套接字
    close(client_fd);
    close(listen_fd);

    return 0;
}

// (base) wyh@wyh-VirtualBox:/media/sf_System-C/Edge 3$ strace -c ./d8_socket
// [Server] Total bytes received: 4, time: 5022 ms
// % time     seconds  usecs/call     calls    errors syscall
// ------ ----------- ----------- --------- --------- ----------------
//  20.33    0.001521          69        22           mmap
//  18.19    0.001361         226         6           mprotect
//   9.24    0.000691         691         1           execve
//   8.26    0.000618         618         1           futex
//   7.37    0.000551          91         6           fstat
//   7.33    0.000548         109         5           write
//   4.79    0.000358          71         5           openat
//   4.01    0.000300          42         7           close
//   4.00    0.000299          99         3           recvfrom
//   2.54    0.000190         190         1           munmap
//   2.37    0.000177          44         4           read
//   2.34    0.000175          58         3           brk
//   1.36    0.000102          51         2           pread64
//   1.27    0.000095          95         1           socket
//   1.16    0.000087          87         1           accept
//   0.90    0.000067          67         1           bind
//   0.83    0.000062          62         1           set_robust_list
//   0.83    0.000062          62         1           rseq
//   0.64    0.000048          48         1           listen
//   0.63    0.000047          47         1         1 access
//   0.56    0.000042          42         1           arch_prctl
//   0.56    0.000042          42         1           set_tid_address
//   0.51    0.000038          38         1           prlimit64
//   0.00    0.000000           0         1           getrandom
// ------ ----------- ----------- --------- --------- ----------------
// 100.00    0.007481          97        77         1 total

// (base) wyh@wyh-VirtualBox:/media/sf_System-C/Edge 3$ strace -c ./d8_socket
// [Server] Total bytes received: 6, time: 3170 ms
// % time     seconds  usecs/call     calls    errors syscall
// ------ ----------- ----------- --------- --------- ----------------
//  63.83    0.189478         200       947           clock_nanosleep
//  33.48    0.099392         104       951       947 recvfrom
//   0.69    0.002034        2033         1           execve
//   0.67    0.001998          90        22           mmap
//   0.22    0.000645         107         6           write
//   0.15    0.000459          76         6           mprotect
//   0.15    0.000454         454         1         1 access
//   0.15    0.000453          64         7           close
//   0.15    0.000437          72         6           fstat
//   0.09    0.000282          70         4           read
//   0.09    0.000278          55         5           openat
//   0.08    0.000233          77         3           brk
//   0.04    0.000127         127         1           accept
//   0.03    0.000096          48         2           pread64
//   0.03    0.000078          78         1           munmap
//   0.02    0.000072          72         1           getrandom
//   0.02    0.000067          67         1           bind
//   0.02    0.000053          53         1           socket
//   0.02    0.000050          50         1           rseq
//   0.02    0.000045          45         1           listen
//   0.01    0.000044          44         1           futex
//   0.01    0.000042          42         1           set_robust_list
//   0.01    0.000038          38         1           set_tid_address
//   0.00    0.000007           7         1           arch_prctl
//   0.00    0.000006           3         2           fcntl
//   0.00    0.000000           0         1           prlimit64
// ------ ----------- ----------- --------- --------- ----------------
// 100.00    0.296868         150      1975       948 total