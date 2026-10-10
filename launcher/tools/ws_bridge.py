#!/usr/bin/env python3
"""Local WebSocket bridge for testing SteamLauncherKit on Linux.

Swift's Foundation on Linux has no WebSocket client, so `steamctl` on Linux talks to this
bridge on 127.0.0.1 with 4-byte big-endian length-prefixed frames. The first frame is the
wss:// URL to open; every later frame is one binary WebSocket message in either direction.
Apple platforms use URLSessionWebSocketTask directly and never need this.

    pip install websocket-client && python3 tools/ws_bridge.py [port]
"""
import os
import socket
import struct
import sys
import threading
from urllib.parse import urlparse

import websocket


def read_exact(conn, n):
    buf = b""
    while len(buf) < n:
        part = conn.recv(n - len(buf))
        if not part:
            raise EOFError
        buf += part
    return buf


def read_frame(conn):
    (n,) = struct.unpack(">I", read_exact(conn, 4))
    return read_exact(conn, n)


def serve(conn):
    try:
        url = read_frame(conn).decode()
        proxy = urlparse(os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy") or "")
        opts = {}
        if proxy.hostname:
            opts = dict(http_proxy_host=proxy.hostname, http_proxy_port=proxy.port, proxy_type="http")
        ca = os.environ.get("SSL_CERT_FILE") or ("/root/.ccr/ca-bundle.crt" if os.path.exists("/root/.ccr/ca-bundle.crt") else None)
        ws = websocket.create_connection(url, sslopt={"ca_certs": ca} if ca else {}, timeout=30, **opts)
        ws.settimeout(None)
    except Exception as e:  # noqa: BLE001
        print("bridge: connect failed:", e, file=sys.stderr)
        conn.close()
        return

    def up():
        try:
            while True:
                ws.send_binary(read_frame(conn))
        except Exception:  # noqa: BLE001
            ws.close()

    threading.Thread(target=up, daemon=True).start()
    try:
        while True:
            opcode, data = ws.recv_data()
            if opcode == websocket.ABNF.OPCODE_CLOSE:
                break
            conn.sendall(struct.pack(">I", len(data)) + data)
    except Exception:  # noqa: BLE001
        pass
    conn.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 27099
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen()
    print(f"ws bridge on 127.0.0.1:{port}", flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
