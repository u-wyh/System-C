# 基础镜像
FROM docker.xuanyuan.run/library/ubuntu:latest

# 安装编译工具 + nlohmann json 系统库
RUN apt update && apt install -y g++ make nlohmann-json3-dev

# 设置工作目录
WORKDIR /app

# 拷贝源代码
COPY d19_server.cpp /app/

# 编译程序
RUN g++ d19_server.cpp -o server -std=c++17 -pthread -O2

# 暴露端口
EXPOSE 8080

# 默认环境变量
ENV SERVER_PORT=8080
ENV THREAD_NUM=4

# 启动服务
CMD ["./server"]