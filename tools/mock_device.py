#!/usr/bin/env python3
"""Mock PAW3395 Wi-Fi monitor for testing the web app without hardware.

Serves main/web/* and implements the same HTTP + WebSocket protocol v1 as the
phase 5 firmware (see main/motion_protocol.h).  Python standard library only.

    python tools/mock_device.py --port 8080
    -> open http://127.0.0.1:8080/ in Chrome

Fault injection for testing the browser side:
    --drop-every N        do not send every N-th packet (samples are lost)
    --disconnect-every S  close the WebSocket every S seconds (tests resume)
    --stall-at S          stop sending for --stall-seconds once, S seconds after connect
"""

import argparse
import asyncio
import base64
import collections
import hashlib
import json
import math
import os
import random
import struct
import time

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main", "web")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PROTOCOL_VERSION = 1
HEADER = struct.Struct("<2sBBBBHII")
SAMPLE = struct.Struct("<IIhhBBBB")
MAX_SAMPLES = 64
CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
}


class Sampler:
    """Generates samples on a fixed schedule into a ring buffer, like motion_logger."""

    def __init__(self, period_ms, ring_samples):
        self.period_us = period_ms * 1000
        self.ring_samples = ring_samples
        self.ring = [None] * ring_samples
        self.next_seq = 0
        self.t0 = time.monotonic()
        self.boot_us = random.randint(5_000_000, 20_000_000)
        self.window = collections.deque()

    def now_us(self):
        return self.boot_us + int((time.monotonic() - self.t0) * 1e6)

    def produce_until_now(self):
        due = int((time.monotonic() - self.t0) * 1e6) // self.period_us
        if due - self.next_seq >= self.ring_samples:
            # Samples older than the ring would be overwritten anyway; only their sequence numbers advance.
            self.next_seq = due - self.ring_samples + 1
        while self.next_seq <= due:
            seq = self.next_seq
            ts = self.boot_us + seq * self.period_us + random.randint(0, 40)
            t = ts / 1e6
            # Car-like pattern: forward speed oscillates, lateral slips during turns.
            dx = int(round(150 * max(0.0, math.sin(2 * math.pi * t / 6.0)) + random.gauss(0, 2)))
            dy = int(round(30 * math.sin(2 * math.pi * t / 2.0) * (dx > 20) + random.gauss(0, 1)))
            squal = max(0, min(255, int(round(105 + 15 * math.sin(2 * math.pi * t / 9.0) + random.gauss(0, 2)))))
            motion = 0x80 if (dx or dy) else 0x00
            self.ring[seq % self.ring_samples] = (seq, ts & 0xFFFFFFFF, dx, dy, squal, motion, 0x02)
            self.window.append(ts)
            self.next_seq += 1
        while len(self.window) > 2 and self.window[-1] - self.window[0] > 1_000_000:
            self.window.popleft()

    def read(self, cursor, max_samples):
        """Return (samples, new_cursor, lost) with the same clamping as motion_logger_read()."""
        retained = min(self.next_seq, self.ring_samples)
        oldest = self.next_seq - retained
        lost = 0
        if cursor > self.next_seq:
            cursor = self.next_seq
        elif cursor < oldest:
            lost = oldest - cursor
            cursor = oldest
        count = min(self.next_seq - cursor, max_samples)
        samples = [self.ring[(cursor + i) % self.ring_samples] for i in range(count)]
        return samples, cursor + count, lost


def encode_packet(packet_seq, period_us, samples):
    out = bytearray(HEADER.pack(b"PW", PROTOCOL_VERSION, HEADER.size, SAMPLE.size, len(samples), 0,
                                packet_seq & 0xFFFFFFFF, period_us))
    for s in samples:
        out += SAMPLE.pack(*s, 0)
    return bytes(out)


def ws_frame(opcode, payload):
    header = bytearray([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header += bytes([126]) + struct.pack(">H", n)
    else:
        header += bytes([127]) + struct.pack(">Q", n)
    return bytes(header) + payload


async def read_ws_frame(reader):
    b0, b1 = await reader.readexactly(2)
    opcode = b0 & 0x0F
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack(">H", await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", await reader.readexactly(8))[0]
    mask = await reader.readexactly(4) if b1 & 0x80 else b"\0\0\0\0"
    data = bytearray(await reader.readexactly(n))
    for i in range(n):
        data[i] ^= mask[i % 4]
    return opcode, bytes(data)


class MockDevice:
    def __init__(self, args):
        self.args = args
        self.sampler = Sampler(args.period_ms, args.ring_samples)
        self.boot_id = "%08x" % random.getrandbits(32)
        self.active_writer = None

    async def handle(self, reader, writer):
        try:
            request = await reader.readuntil(b"\r\n\r\n")
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            writer.close()
            return
        lines = request.decode("latin-1").split("\r\n")
        method, path, _ = lines[0].split(" ", 2)
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        path = path.split("?", 1)[0]
        if path == "/ws" and headers.get("upgrade", "").lower() == "websocket":
            await self.websocket(reader, writer, headers)
            return
        await self.http(writer, method, path)

    async def http(self, writer, method, path):
        if path == "/":
            path = "/index.html"
        status, ctype, body = "404 Not Found", "text/plain", b"not found"
        if path == "/api/info":
            info = {"firmware": "mock-device", "protocol": PROTOCOL_VERSION, "boot_id": self.boot_id,
                    "uptime_ms": self.sampler.now_us() // 1000, "ssid": "PAW3395-MONITOR",
                    "ip": "127.0.0.1", "stations": 1, "rssi_dbm": -42,
                    "period_us": self.sampler.period_us, "synthetic": True}
            status, ctype, body = "200 OK", "application/json", json.dumps(info).encode()
        elif path == "/favicon.ico":
            status, ctype, body = "204 No Content", "text/plain", b""
        else:
            name = os.path.basename(path)
            file = os.path.join(WEB_DIR, name)
            ext = os.path.splitext(name)[1]
            if ext in CONTENT_TYPES and os.path.isfile(file):
                with open(file, "rb") as f:
                    body = f.read()
                status, ctype = "200 OK", CONTENT_TYPES[ext]
        writer.write(("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n" % (status, ctype, len(body))).encode() + body)
        await writer.drain()
        writer.close()

    async def websocket(self, reader, writer, headers):
        accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + WS_GUID).encode()).digest())
        writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        await writer.drain()
        if self.active_writer is not None:
            self.active_writer.close()
        self.active_writer = writer
        print("WebSocket client connected")

        state = {"cursor": None}

        async def receive():
            while True:
                opcode, data = await read_ws_frame(reader)
                if opcode == 0x8:
                    return
                if opcode == 0x9:
                    writer.write(ws_frame(0xA, data))
                elif opcode == 0x1:
                    message = json.loads(data.decode())
                    if message.get("cmd") == "stream":
                        self.sampler.produce_until_now()
                        state["cursor"] = message.get("from", self.sampler.next_seq)
                        print("stream requested from", state["cursor"])

        receiver = asyncio.ensure_future(receive())
        s = self.sampler
        s.produce_until_now()
        hello = {"type": "hello", "protocol": PROTOCOL_VERSION, "firmware": "mock-device", "boot_id": self.boot_id,
                 "period_us": s.period_us, "packet_samples": self.args.packet_samples,
                 "ring_samples": s.ring_samples, "synthetic": True, "next_seq": s.next_seq,
                 "ssid": "PAW3395-MONITOR"}
        writer.write(ws_frame(0x1, json.dumps(hello).encode()))
        packet_seq = 0
        packets = 0
        ring_lost = 0
        connected_at = time.monotonic()
        next_status = time.monotonic()
        stalled = False
        try:
            while not receiver.done() and self.active_writer is writer:
                await asyncio.sleep(s.period_us * self.args.packet_samples / 1e6)
                s.produce_until_now()
                elapsed = time.monotonic() - connected_at
                if self.args.disconnect_every and elapsed > self.args.disconnect_every:
                    print("injected disconnect")
                    break
                if self.args.stall_at and not stalled and elapsed > self.args.stall_at:
                    stalled = True
                    print("injected stall")
                    await asyncio.sleep(self.args.stall_seconds)
                    s.produce_until_now()
                if state["cursor"] is not None:
                    while True:
                        samples, state["cursor"], lost = s.read(state["cursor"], MAX_SAMPLES)
                        ring_lost += lost
                        if not samples:
                            break
                        skip = self.args.drop_every and (packet_seq + 1) % self.args.drop_every == 0
                        if not skip:
                            writer.write(ws_frame(0x2, encode_packet(packet_seq, s.period_us, samples)))
                            packets += 1
                        packet_seq += 1
                        if len(samples) < MAX_SAMPLES:
                            break
                if time.monotonic() >= next_status:
                    next_status = time.monotonic() + 1.0
                    w = list(s.window)
                    periods = [b - a for a, b in zip(w, w[1:])] or [0]
                    status = {"type": "status", "boot_id": self.boot_id, "uptime_ms": s.now_us() // 1000,
                              "next_seq": s.next_seq, "window_samples": max(0, len(w) - 1),
                              "window_us": (w[-1] - w[0]) if len(w) > 1 else 0,
                              "motion_samples": max(0, len(w) - 1), "min_period_us": min(periods),
                              "max_period_us": max(periods), "max_read_us": 210, "late_reads": 0,
                              "read_errors": 0,
                              "backlog": 0 if state["cursor"] is None else s.next_seq - state["cursor"],
                              "ring_lost": ring_lost, "packets": packets, "send_failures": 0,
                              "stations": 1, "rssi_dbm": -42, "free_heap": 180000, "min_free_heap": 170000}
                    writer.write(ws_frame(0x1, json.dumps(status).encode()))
                await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            receiver.cancel()
            writer.close()
            if self.active_writer is writer:
                self.active_writer = None
            print("WebSocket client closed")


async def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--period-ms", type=int, default=10)
    parser.add_argument("--packet-samples", type=int, default=5)
    parser.add_argument("--ring-samples", type=int, default=2048)
    parser.add_argument("--drop-every", type=int, default=0)
    parser.add_argument("--disconnect-every", type=float, default=0)
    parser.add_argument("--stall-at", type=float, default=0)
    parser.add_argument("--stall-seconds", type=float, default=5)
    args = parser.parse_args()
    device = MockDevice(args)
    server = await asyncio.start_server(device.handle, args.host, args.port)
    print("mock device on http://%s:%d/  (boot_id=%s)" % (args.host, args.port, device.boot_id))
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
