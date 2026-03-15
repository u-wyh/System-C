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