"""Accepts TCP connections on the given ports and logs everything received."""
import socket
import sys
import threading
import time

LOG = open(r"C:\aot2\tools\capture.log", "a", buffering=1)


def log(msg):
    LOG.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")


def handle(conn, addr, port):
    log(f"port {port}: connection from {addr}")
    conn.settimeout(60)
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            log(f"port {port}: {len(data)} bytes")
            for i in range(0, len(data), 32):
                chunk = data[i:i + 32]
                log("  " + chunk.hex(" ") + "  " + "".join(chr(b) if 32 <= b < 127 else "." for b in chunk))
    except Exception as e:
        log(f"port {port}: {e}")
    log(f"port {port}: closed")
    conn.close()


def serve(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(8)
    log(f"listening on {port}")
    while True:
        c, a = s.accept()
        threading.Thread(target=handle, args=(c, a, port), daemon=True).start()


for p in sys.argv[1:]:
    threading.Thread(target=serve, args=(int(p),), daemon=True).start()
while True:
    time.sleep(1)
