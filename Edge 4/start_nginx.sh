#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

SUDO=""
if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
fi

pkill -f "server" 2>/dev/null || $SUDO pkill -f "server" 2>/dev/null || true
for _ in $(seq 1 50); do
    if ! pgrep -f "server" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

$SUDO rm -f server-direct.log server-8080.log server-8082.log nginx-8080.out nginx-8082.out direct.out cluster-8080.out cluster-8082.out

g++ -O2 -std=c++17 server.cpp -o server -lpthread
g++ -O2 -std=c++17 client.cpp -o client -lpthread

: > server-8080.log
: > server-8082.log

nohup ./server --no-watchdog config.json --port 8080 --log-file server-8080.log >/dev/null 2>&1 &
nohup ./server --no-watchdog config.json --port 8082 --log-file server-8082.log >/dev/null 2>&1 &

if command -v nginx >/dev/null 2>&1; then
    if [ -w /etc/nginx ] || command -v sudo >/dev/null 2>&1; then
        SUDO=""
        if [ ! -w /etc/nginx ]; then
            SUDO="sudo"
        fi

        $SUDO mkdir -p /etc/nginx/stream-conf.d
        $SUDO cp nginx-stream.conf /etc/nginx/stream-conf.d/edge4_echo.conf

        if ! $SUDO grep -q "include /etc/nginx/stream-conf.d/\\*\\.conf;" /etc/nginx/nginx.conf; then
            if $SUDO grep -q "^stream\\s*{" /etc/nginx/nginx.conf; then
                $SUDO python3 - <<'PY'
from pathlib import Path
path = Path("/etc/nginx/nginx.conf")
text = path.read_text()
needle = "stream {\n"
insert = "stream {\n    include /etc/nginx/stream-conf.d/*.conf;\n"
if needle in text and "include /etc/nginx/stream-conf.d/*.conf;" not in text:
    text = text.replace(needle, insert, 1)
    path.write_text(text)
PY
            else
                cat <<'EOF' | $SUDO tee -a /etc/nginx/nginx.conf >/dev/null

stream {
    include /etc/nginx/stream-conf.d/*.conf;
}
EOF
            fi
        fi

        $SUDO nginx -t
        if command -v systemctl >/dev/null 2>&1; then
            $SUDO systemctl reload nginx
        else
            $SUDO nginx -s reload
        fi
    else
        echo "nginx found, but /etc/nginx needs root permission."
        echo "please rerun with sudo or configure nginx manually."
        exit 1
    fi
else
    echo "nginx is not installed."
    exit 1
fi

echo "nginx mode started: backends 127.0.0.1:8080, 127.0.0.1:8082"
echo "public entry: 127.0.0.1:8081"
echo "log files: server-8080.log server-8082.log"
