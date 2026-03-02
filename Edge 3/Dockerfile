FROM docker.xuanyuan.run/library/ubuntu:latest

# 拷贝你的源代码到容器
COPY d13_log.cpp /app/
WORKDIR /app

# 安装编译工具
RUN apt update && apt install -y g++ make

# 编译你的程序
RUN g++ d13_log.cpp -o d13_log

# 容器启动时运行程序
CMD ["./d13_log"]