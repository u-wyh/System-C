# 高并发 AI 问答服务器

基于 Linux epoll ET 模式 + Reactor 事件驱动模型实现的高并发 TCP 服务器，接入阿里云通义千问 API，支持多客户端并发智能问答及多轮上下文对话。

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
                    │  ThreadPool  │  线程池，异步处理请求
                    └──────┬──────┘
                           │ HTTP POST
                    ┌──────▼──────┐
                    │  AiClient   │  调用通义千问 API
                    └─────────────┘
```

**核心模块：**

- **EpollServer**：epoll ET 模式处理并发连接，非阻塞读写，解决粘包问题
- **ThreadPool**：线程池任务调度，AI 请求异步执行不阻塞主线程
- **AsyncLogger**：双缓冲异步日志，I/O 与业务逻辑解耦
- **Watchdog**：守护进程机制，子进程崩溃后自动重启
- **ConfigManager**：监听 config.json 文件变更，支持热更新无需重启
- **AiSemaphore**：信号量限流，控制最大并发 AI 请求数

## 环境依赖

```bash
# Ubuntu / Debian
sudo apt install -y build-essential libssl-dev nlohmann-json3-dev

# 下载 cpp-httplib（header-only）
wget https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
```

## 编译

```bash
g++ -O2 -std=c++17 server.cpp -o ai_server \
    -lssl -lcrypto -lpthread \
    -DCPPHTTPLIB_OPENSSL_SUPPORT
```

## 配置

复制并编辑配置文件：

```bash
cp config.json.example config.json
vim config.json
```

`config.json` 各字段说明：

```json
{
    "thread_num": 4,              // 线程池线程数
    "log_level": "INFO",          // 日志等级：DEBUG / INFO / WARN / ERROR
    "ai_api_key": "sk-xxxxxxxx",  // 阿里云 DashScope API Key（必填）
    "ai_model": "qwen-turbo",     // 模型名称，可选 qwen-plus / qwen-max
    "ai_timeout_s": 30,           // AI 请求超时时间（秒）
    "ai_max_concurrent": 8,       // 最大并发 AI 请求数
    "ai_max_history": 10,         // 每个连接保留的最大对话轮数
    "system_prompt": "You are a helpful assistant. Reply concisely."
}
```

**获取 API Key：**

1. 登录 [阿里云百炼控制台](https://bailian.console.aliyun.com/)
2. 左侧菜单 → 密钥管理 → 创建 API Key
3. 将 Key 填入 `config.json` 的 `ai_api_key` 字段

## 运行

```bash
# 前台运行（可看到日志输出）
./ai_server

# 后台运行
nohup ./ai_server > /dev/null 2>&1 &
```

服务器默认监听 **8080** 端口。

## 测试

**推荐：使用 Python 客户端（UTF-8 编码，支持中文）**

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
        print("AI:", reply.decode('utf-8').strip())
except KeyboardInterrupt:
    print("\n断开连接")
finally:
    s.close()
```

```bash
python3 client.py
```

**也可以用 telnet 测试（仅限英文）：**

```bash
LANG=zh_CN.UTF-8 telnet localhost 8080
```

## 性能

**网络层（纯 echo 模式，不调用 AI）：**

| 配置 | QPS |
|------|-----|
| 本地基础配置（4线程） | 9,000+ |
| 本地优化配置 | 30,000+ |
| 挂载 Nginx 后 | 120,000+ |

测试工具：wrk / ab

**AI 问答模式：**

受限于 AI 接口响应时间（通常 500ms～2s）和限流配置（默认最大 8 个并发 AI 请求），实际问答吞吐约 4～16 QPS。网络层本身不是瓶颈，瓶颈在 AI 接口的响应延迟。

## 热更新

运行期间直接修改 `config.json` 即可生效，无需重启服务器。支持热更新的字段：

- `thread_num`：动态调整线程池大小
- `log_level`：动态调整日志等级
- `ai_model`：切换模型
- `ai_max_concurrent`：调整限流阈值
- `system_prompt`：修改 AI 角色设定

## 日志

运行日志写入当前目录 `server.log`，滚动写入，最多保留 1000 行。

日志格式：`[时间][等级] 内容`

```
[2025-03-15 11:04:31][INFO] [Server] listening on port 8080
[2025-03-15 11:04:31][INFO] [AI] fd=6 question=你好
[2025-03-15 11:04:32][INFO] [AI] fd=6 answer=你好！有什么我可以帮助你的吗？
```