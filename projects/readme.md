# 高并发 Echo 服务器

基于 Linux epoll ET 模式 + Reactor 事件驱动模型实现的高并发 TCP Echo 服务器，支持多客户端并发连接、按行收发消息，并保留日志、监控、热更新和 watchdog 等工程化能力。

## 技术架构

```
                    ┌─────────────┐
                    │  Watchdog   │  父进程，子进程崩溃自动重启
                    └──────┬──────┘
                           │ fork
                    ┌──────▼──────┐
                    │  EpollServer │  epoll ET 模式，非阻塞 I/O
                    └──────┬──────┘
                           │ submit
                    ┌──────▼──────┐
                    │  ThreadPool  │  线程池，异步处理 Echo 任务
                    └─────────────┘
```

**核心模块：**

- **EpollServer**：epoll ET 模式处理并发连接，非阻塞读写，解决粘包问题
- **ThreadPool**：线程池任务调度，Echo 处理不阻塞主线程
- **AsyncLogger**：双缓冲异步日志，I/O 与业务逻辑解耦
- **Watchdog**：守护进程机制，子进程崩溃后自动重启
- **ConfigManager**：监听 config.json 文件变更，支持热更新无需重启

## 环境依赖

```bash
# Ubuntu / Debian
sudo apt install -y build-essential nlohmann-json3-dev
```

## 编译

```bash
g++ -O2 -std=c++17 server.cpp -o echo_server -lpthread
```

## 配置

`config.json` 各字段说明：

```json
{
    "thread_num": 4,      // 线程池线程数
    "log_level": "INFO"   // 日志等级：DEBUG / INFO / WARN / ERROR
}
```

## 运行

```bash
# 前台运行（可看到日志输出）
./echo_server

# 后台运行
nohup ./echo_server > /dev/null 2>&1 &
```

服务器默认监听 **8080** 端口。

## 测试

**推荐：使用 Python 客户端测试按行 Echo**

```python
# client.py
import socket

HOST = '127.0.0.1'  # 替换为服务器 IP
PORT = 8080

s = socket.socket()
s.connect((HOST, PORT))
print(f"已连接到 {HOST}:{PORT}，输入问题后回车，Ctrl+C 退出\n")

try:
    while True:
        msg = input("你: ").strip()
        if not msg:
            continue
        s.sendall((msg + '\n').encode('utf-8'))
        reply = b''
        while not reply.endswith(b'\n'):
            chunk = s.recv(4096)
            if not chunk:
                break
            reply += chunk
        print("Echo:", reply.decode('utf-8').strip())
except KeyboardInterrupt:
    print("\n断开连接")
finally:
    s.close()
```

```bash
python3 client.py
```

**也可以用 telnet 测试：**

```bash
LANG=zh_CN.UTF-8 telnet localhost 8080
```

## 性能

| 配置 | QPS |
|------|-----|
| 本地基础配置（4线程） | 9,000+ |
| 本地优化配置 | 30,000+ |
| 挂载 Nginx 后 | 120,000+ |

测试工具：wrk / ab

## 热更新

运行期间直接修改 `config.json` 即可生效，无需重启服务器。支持热更新的字段：

- `thread_num`：动态调整线程池大小
- `log_level`：动态调整日志等级

## 日志

运行日志写入当前目录 `server.log`，滚动写入，最多保留 1000 行。

日志格式：`[时间][等级] 内容`

```
[2025-03-15 11:04:31][INFO] [Server] listening on port 8080
[2025-03-15 11:04:31][INFO] [Echo] fd=6 message=hello
[2025-03-15 11:04:31][INFO] [Monitor] CPU=1.2% Mem=8.5MB TPS=13500
```
