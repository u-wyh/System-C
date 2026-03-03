FROM docker.xuanyuan.run/library/ubuntu:latest

# 拷贝你的源代码到容器
COPY d16_nginx.cpp /app/
WORKDIR /app

# 安装编译工具
RUN apt update && apt install -y g++ make

# 编译你的程序
RUN g++ d16_nginx.cpp -o d16_nginx

# 容器启动时运行程序
CMD ["./d16_nginx"]