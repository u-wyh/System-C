#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

SUDO=""
if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
fi

pkill -f "echo_server" 2>/dev/null || $SUDO pkill -f "echo_server" 2>/dev/null || true
for _ in $(seq 1 50); do
    if ! pgrep -f "echo_server" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

$SUDO rm -f server-direct.log server-8080.log server-8082.log server-direct.log nginx-8080.out nginx-8082.out direct.out cluster-8080.out cluster-8082.out

g++ -O2 -std=c++17 server.cpp -o echo_server -lpthread
g++ -O2 -std=c++17 client.cpp -o client -lpthread

: > server-8080.log

nohup ./echo_server --no-watchdog config.json --port 8080 --log-file server-8080.log >/dev/null 2>&1 &

echo "direct server started on 127.0.0.1:8080"
echo "log file: server-8080.log"
