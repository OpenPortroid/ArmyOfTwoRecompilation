"""Minimal EA FESL / Theater emulator for Army of Two (Xbox 360) research.

Packet: 4-byte type, u32 BE (flags|tid), u32 BE total length, body of
"key=value\n" lines terminated by NUL.
"""
import socket
import struct
import sys
import time

LOG = open(r"C:\aot2\tools\ea_emu.log", "a", buffering=1)
CFG = {
    "host_ip": sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1",
    "fesl_port": 18480,
    "theater_port": 18485,
    "messenger_port": 13505,
}


import threading as _threading

# Shared matchmaking state (survives handler hot-reloads).
STATE = {
    "lock": _threading.RLock(),
    "games": {},        # gid -> dict
    "next_gid": 1,
    "next_pid": 1,
    "theaters": {},     # uid -> Theater connection object
    "pending": {},      # pid -> joining Theater connection
}


def log(msg):
    LOG.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")


def parse(body):
    out = {}
    for line in body.rstrip(b"\x00").decode("latin1").split("\n"):
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out


def build(ptype, flags, fields):
    body = "".join(f"{k}={v}\n" for k, v in fields.items()).encode("latin1") + b"\x00"
    return ptype + struct.pack(">II", flags, 12 + len(body)) + body


def recv_packets(conn, buf):
    while True:
        while len(buf) >= 12:
            length = struct.unpack(">I", buf[8:12])[0]
            if len(buf) < length:
                break
            pkt, buf = buf[:length], buf[length:]
            yield pkt[:4], struct.unpack(">I", pkt[4:8])[0], parse(pkt[12:])
        data = conn.recv(65536)
        if not data:
            return
        buf += data


