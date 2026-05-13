import signal
import socket
import sys
import threading
import time


running = True
sent = 0
received = 0
connect_ok = 0
connect_fail = 0
counter_lock = threading.Lock()


def signal_handler(signum, frame):
    del signum, frame
    global running
    running = False


def parse_args():
    host = "127.0.0.1"
    port = 8080
    clients = 100
    messages_per_client = 1000
    interval_ms = 0

    if len(sys.argv) > 1 and sys.argv[1] == "--help":
        print(
            "Usage: python qps_client.py [host] [port] [clients] "
            "[messages_per_client] [interval_ms]"
        )
        sys.exit(0)

    if len(sys.argv) > 1:
        host = sys.argv[1]
    if len(sys.argv) > 2:
        port = int(sys.argv[2])
    if len(sys.argv) > 3:
        clients = int(sys.argv[3])
    if len(sys.argv) > 4:
        messages_per_client = int(sys.argv[4])
    if len(sys.argv) > 5:
        interval_ms = int(sys.argv[5])

    return host, port, clients, messages_per_client, interval_ms


def recv_line(sock):
    data = bytearray()
    while running:
        chunk = sock.recv(1)
        if not chunk:
            return None
        data.extend(chunk)
        if chunk == b"\n":
            return bytes(data)
    return None


def worker(host, port, worker_id, messages_per_client, interval_ms):
    global sent, received, connect_ok, connect_fail
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
    except OSError:
        with counter_lock:
            connect_fail += 1
        return

    with counter_lock:
        connect_ok += 1

    try:
        for i in range(messages_per_client):
            if not running:
                break
            if interval_ms > 0:
                time.sleep(interval_ms / 1000.0)

            msg = f"msg {worker_id}:{i}\n".encode("utf-8")
            sock.sendall(msg)
            with counter_lock:
                sent += 1

            reply = recv_line(sock)
            if reply is None:
                break
            with counter_lock:
                received += 1
    finally:
        sock.close()


def monitor(total_target):
    last_received = 0
    sec = 0
    global running
    while running:
        time.sleep(1)
        with counter_lock:
            cur_sent = sent
            cur_received = received
            cur_ok = connect_ok
            cur_fail = connect_fail
        qps = cur_received - last_received
        sec += 1
        print(
            f"[QPS] sec={sec} sent={cur_sent} received={cur_received} "
            f"qps={qps} connect_ok={cur_ok} connect_fail={cur_fail}"
        )
        last_received = cur_received
        if cur_received >= total_target:
            break


def main():
    global running
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    host, port, clients, messages_per_client, interval_ms = parse_args()
    print(
        f"[Client] host={host} port={port} clients={clients} "
        f"messages_per_client={messages_per_client} interval_ms={interval_ms}"
    )

    total_target = clients * messages_per_client
    start = time.perf_counter()

    threads = []
    for i in range(1, clients + 1):
        thread = threading.Thread(
            target=worker,
            args=(host, port, i, messages_per_client, interval_ms),
        )
        thread.start()
        threads.append(thread)

    monitor_thread = threading.Thread(target=monitor, args=(total_target,))
    monitor_thread.start()

    for thread in threads:
        thread.join()

    running = False
    monitor_thread.join()

    elapsed = time.perf_counter() - start
    with counter_lock:
        total_sent = sent
        total_received = received
        total_ok = connect_ok
        total_fail = connect_fail
    avg_qps = total_received / elapsed if elapsed > 0 else 0.0

    print(
        f"[Summary] elapsed={elapsed:.3f}s sent={total_sent} received={total_received} "
        f"avg_qps={avg_qps:.2f} connect_ok={total_ok} connect_fail={total_fail}"
    )


if __name__ == "__main__":
    main()
