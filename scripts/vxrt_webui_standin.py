#!/usr/bin/env python3
"""Stands in for a game's SteamVR web UI when SteamVR runs on VXRuntime.

SteamVR shows a game's web UI (Half-Life: Alyx's loading screens, game/hlvr/webui) in vrwebhelper, a Chromium
program that does not run on VXRuntime. The page talks to the game through SteamVR's "vrmailbox": a WebSocket to
vrserver's web server (ws://localhost:27062; pages vrserver serves itself need no secret, so this one comes from
that origin), where it opens its own mailbox and tells the game's mailbox it is
"ready". Alyx gives up after a minute without that message ("Timed out waiting for response from Mongoose").

This script does what the page does at start: opens the page's mailbox, waits until the game's mailbox is
registered, and sends {"type": "ready"}; then it waits for the next registration (the game restarting) and does it
again (once per game: a second "ready" during a level load can leave the game paused). The loading screens
themselves are not shown.

    vxrt_webui_standin.py [page mailbox] [game mailbox]      (default: hlvr/interstitials hlvr/mailbox)
"""
import base64
import json
import os
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", 27062
PAGE = sys.argv[1] if len(sys.argv) > 1 else "hlvr/interstitials"
GAME = sys.argv[2] if len(sys.argv) > 2 else "hlvr/mailbox"


def log(text):
    print(f"[webui stand-in] {text}", flush=True)


def connect():
    s = socket.create_connection((HOST, PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: localhost:{PORT}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
               f"Origin: http://localhost:{PORT}\r\n\r\n").encode())
    reply = b""
    while b"\r\n\r\n" not in reply:
        chunk = s.recv(4096)
        if not chunk:
            raise ConnectionError("closed during the handshake")
        reply += chunk
    if b" 101 " not in reply.split(b"\r\n")[0]:
        raise ConnectionError(reply.split(b"\r\n")[0].decode(errors="replace"))
    s.settimeout(None)
    return s


def send_text(s, text):
    data = text.encode()
    head = bytes([0x81])                                   # FIN, text
    mask = os.urandom(4)                                   # clients mask their frames
    if len(data) < 126:
        head += bytes([0x80 | len(data)])
    elif len(data) < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", len(data))
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", len(data))
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def recv_exact(s, n):
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        if not chunk:
            raise ConnectionError("closed")
        out += chunk
    return out


def recv_text(s):
    while True:
        b0, b1 = recv_exact(s, 2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", recv_exact(s, 2))[0]
        elif n == 127:
            n = struct.unpack(">Q", recv_exact(s, 8))[0]
        mask = recv_exact(s, 4) if b1 & 0x80 else None
        data = recv_exact(s, n)
        if mask:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        op = b0 & 0x0F
        if op == 8:
            raise ConnectionError("closed by the server")
        if op == 9:                                        # ping: pong
            s.sendall(bytes([0x8A, 0x80 | len(data)]) + b"\0\0\0\0" + data)
        if op == 1:
            return data.decode(errors="replace")


def wait_for(s, request, mailbox, message_id):
    """Asks vrserver to tell when `mailbox` is registered (or gone) and waits for that answer; other messages
    (the game's to the page: paths, loading screens) are logged and otherwise ignored, as the page shows nothing."""
    message = {"type": request, "mailbox_name": mailbox, "returnAddress": PAGE, "message_id": message_id}
    send_text(s, "mailbox_send web_server_mailbox " + json.dumps(message))
    while True:
        try:
            reply = json.loads(recv_text(s))
        except ValueError:
            continue
        if reply.get("message_id") == message_id:
            return reply
        log(f"message for the page: {json.dumps(reply)[:300]}")


def serve(s):
    send_text(s, f"mailbox_open {PAGE}")
    message_id = 1
    while True:
        wait_for(s, "request_mailbox_registration_notification", GAME, message_id)
        send_text(s, f"mailbox_send {GAME} " + json.dumps({"type": "ready"}))
        log(f"{GAME} is registered: told it {PAGE} is ready")
        # Once per game: the next "ready" waits until this game's mailbox is gone (the game quit).
        wait_for(s, "request_mailbox_unavailable_notification", GAME, message_id + 1)
        log(f"{GAME} is gone")
        message_id += 2


def main():
    while True:
        try:
            s = connect()
            log(f"connected to vrserver's web server, mailbox {PAGE}")
            serve(s)
        except (OSError, ConnectionError) as e:
            time.sleep(2)


if __name__ == "__main__":
    main()
