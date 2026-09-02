#!/usr/bin/env python3
"""MEC simulator for the VMC thin client.

Emulates the two server-side roles the thin client talks to:

  1. Mapper   — UDP discovery service. The thin client sends
                "VMC1 <device-id>" and receives "VMC1 ROUTE <ip>:<port>"
                pointing at this host's container media port.
  2. Container — the media plane. Streams REAL H.264 video (encoded by a
                local ffmpeg subprocess from a moving test pattern), echoes
                CONTROL keepalives, and ingests INPUT batches. Frames larger
                than the datagram cap are fragmented with the VMC video
                fragment header; ~1% of datagrams are dropped to exercise the
                client's loss handling.

Usage: python3 mec_sim.py [bind_host] [mapper_port] [media_port] [width] [height]
Defaults: 127.0.0.1 9999 6000 1024 768
"""
import os
import random
import select
import socket
import struct
import subprocess
import sys
import threading
import time

MAGIC = 0x5643
VERSION = 1
HEADER = 17

STREAM_CONTROL = 0
STREAM_VIDEO = 1
STREAM_AUDIO = 2
STREAM_INPUT = 3
STREAM_TELEMETRY = 4

FLAG_FEC = 1 << 0
FLAG_KEYFRAME = 1 << 1
FLAG_FRAGMENTED = 1 << 2

FRAG_CHUNK = 1400
FRAG_LAST = 0x8000
FRAG_IDX_MASK = 0x7FFF

DROP_RATE = 0.001

H264_NAL_TYPES = {1, 2, 3, 4, 5}  # VCL slice types


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def pack(stream: int, seq: int, ts_us: int, flags: int = 0,
         payload: bytes = b"") -> bytes:
    hdr = struct.pack("<HBBBBHIIB", MAGIC, VERSION, flags, stream, 0,
                      len(payload), seq & 0xFFFFFFFF, ts_us & 0xFFFFFFFF, 0)
    crc = crc8(hdr[:-1] + payload)
    return hdr[:-1] + bytes([crc]) + payload


def parse(data: bytes):
    if len(data) < HEADER:
        return None
    magic, version, flags, stream, _, plen, seq, ts, crc = struct.unpack(
        "<HBBBBHIIB", data[:HEADER])
    payload = data[HEADER:HEADER + plen]
    if magic != MAGIC or version != VERSION:
        return None
    if crc != crc8(data[:HEADER - 1] + payload):
        return None
    return stream, flags, seq, ts, payload


class H264FrameSource:
    """Reads H.264 Annex-B NAL units from an ffmpeg pipe and groups them
    into access units (frames)."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self._buf = b""
        self._au = b""
        self._pending = b""
        self._au_key = False
        self.proc = None

    def start(self):
        # Bitrate scales with pixel count (~0.1 bit/px/frame @ 30 fps).
        bitrate = max(5_000_000, self.width * self.height * 3)
        maxrate = int(bitrate * 1.2)
        bufsize = int(bitrate * 0.5)
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-re", "-f", "lavfi",
            "-i", f"testsrc2=size={self.width}x{self.height}:rate=30",
            "-c:v", "h264_nvenc",
            "-preset", "p1", "-tune", "ll",
            "-rc", "vbr", "-b:v", str(bitrate), "-maxrate", str(maxrate),
            "-bufsize", str(bufsize),
            "-g", "30", "-keyint_min", "30", "-b_ref_mode", "0",
            "-pix_fmt", "yuv420p",
            "-f", "h264", "pipe:1",
        ]
        print(f"[container] starting encoder: ffmpeg testsrc2 {self.width}x{self.height} "
              f"@30 -> h264_nvenc (GPU) ~{bitrate // 1_000_000} Mbps")
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL)
        return self.proc is not None

    def _find_start(self, buf, from_idx=0):
        n = len(buf)
        for j in range(from_idx, n - 3):
            if buf[j] == 0 and buf[j + 1] == 0 and buf[j + 2] == 1:
                return j, 3
            if (buf[j] == 0 and buf[j + 1] == 0 and buf[j + 2] == 0 and
                    buf[j + 3] == 1):
                return j, 4
        return None, 0

    def _read_more(self, timeout=0.2):
        if self.proc.poll() is not None:
            return False
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not r:
            return False
        data = os.read(self.proc.stdout.fileno(), 65536)
        if not data:
            return False
        self._buf += data
        return True

    def next_nalu(self):
        """Return (start_code_len, nalu_bytes) for the next NAL unit, or
        None if none is available right now."""
        while True:
            pos, slen = self._find_start(self._buf)
            if pos is not None and pos + slen + 1 <= len(self._buf):
                npos, _ = self._find_start(self._buf, pos + slen)
                if npos is not None:
                    body = self._buf[pos + slen:npos]
                    self._buf = self._buf[npos:]
                    return slen, body
                # start code found but not the next one yet
                if pos > 0:
                    self._buf = self._buf[pos:]
            else:
                if pos is None and len(self._buf) > 3:
                    self._buf = self._buf[-3:]  # keep a split start-code tail
            if not self._read_more():
                return None

    def next_au(self):
        """Return the next access unit as (bytes, is_keyframe) or None."""
        while True:
            res = self.next_nalu()
            if res is None:
                if self._au:
                    au, key = self._au, self._au_key
                    self._au = b""
                    self._au_key = False
                    return au, key
                return None
            slen, body = res
            if not body:
                continue
            start = b"\x00\x00\x01" if slen == 3 else b"\x00\x00\x00\x01"
            t = body[0] & 0x1F
            if t in H264_NAL_TYPES:
                if self._au:
                    au, key = self._au, self._au_key
                    self._au = self._pending + start + body
                    self._pending = b""
                    self._au_key = (t == 5)
                    return au, key
                self._au = self._pending + start + body
                self._pending = b""
                self._au_key = (t == 5)
            else:
                if self._au:
                    self._au += start + body
                else:
                    self._pending += start + body


class MECSim:
    def __init__(self, bind_host, mapper_port, media_port, width, height):
        self.bind_host = bind_host
        self.mapper_port = mapper_port
        self.media_port = media_port
        self.media_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.media_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.media_sock.bind((bind_host, media_port))
        self.media_sock.settimeout(0.1)

        self.video_seq = 0
        self.input_rx = 0
        self.control_rx = 0
        self.client_addr = None  # learned from the first client datagram
        self.running = True

        self.source = H264FrameSource(width, height)

    # ---- Mapper service ------------------------------------------------
    def run_mapper(self):
        mapper = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        mapper.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        mapper.bind((self.bind_host, self.mapper_port))
        mapper.settimeout(0.2)
        print(f"[mapper] listening on {self.bind_host}:{self.mapper_port}")
        while self.running:
            try:
                data, addr = mapper.recvfrom(1024)
            except socket.timeout:
                continue
            req = data.decode(errors="replace").strip()
            print(f"[mapper] request from {addr}: {req!r}")
            resp = f"VMC1 ROUTE {self.bind_host}:{self.media_port}"
            mapper.sendto(resp.encode(), addr)
        mapper.close()

    # ---- Video streamer (real H.264) -----------------------------------
    def _send_au(self, data, key):
        seq = self.video_seq
        ts = int(time.monotonic() * 1_000_000)
        if len(data) <= FRAG_CHUNK:
            flags = FLAG_KEYFRAME if key else 0
            if not (random.random() < DROP_RATE):
                self.media_sock.sendto(
                    pack(STREAM_VIDEO, seq, ts, flags, data), self.client_addr)
            self.video_seq = seq + 1
        else:
            frame_id = seq & 0xFFFF
            for off in range(0, len(data), FRAG_CHUNK):
                chunk = data[off:off + FRAG_CHUNK]
                last = off + len(chunk) >= len(data)
                fh = struct.pack("<HH", frame_id,
                                 (off // FRAG_CHUNK) | (FRAG_LAST if last else 0))
                flags = FLAG_FRAGMENTED
                if key:
                    flags |= FLAG_KEYFRAME
                if not (random.random() < DROP_RATE):
                    self.media_sock.sendto(
                        pack(STREAM_VIDEO, seq, ts, flags, fh + chunk),
                        self.client_addr)
                seq += 1
            self.video_seq = seq

    def run_streamer(self):
        print(f"[container] streaming media on {self.bind_host}:{self.media_port}")
        if not self.source.start():
            print("[container] ERROR: ffmpeg failed to start")
            return
        sent = 0
        while self.running:
            if self.client_addr is None:
                time.sleep(0.05)
                continue
            au = self.source.next_au()
            if au is None:
                continue
            data, key = au
            if not data:
                continue
            self._send_au(data, key)
            sent += 1
            if sent % 100 == 0:
                print(f"[container] frames sent={sent} (keyframes) au_bytes={len(data)}")

    # ---- Container ingress (control echo + input) -----------------------
    def run_ingress(self):
        while self.running:
            try:
                data, addr = self.media_sock.recvfrom(4096)
            except socket.timeout:
                continue
            parsed = parse(data)
            if parsed is None:
                continue
            stream, flags, seq, ts, payload = parsed
            if self.client_addr is None:
                print(f"[container] learned client {addr} (stream={stream})")
            self.client_addr = addr  # remember who we stream to
            if stream == STREAM_CONTROL:
                self.control_rx += 1
                # Echo the keepalive back so the client sees link health.
                self.media_sock.sendto(pack(STREAM_CONTROL, seq + 1,
                                            int(time.monotonic() * 1_000_000)),
                                       addr)
            elif stream == STREAM_INPUT:
                self.input_rx += 1
                n = struct.unpack("<H", payload[:2])[0] if len(payload) >= 2 else 0
                if self.input_rx % 25 == 0:
                    print(f"[container] input batch #{self.input_rx}: {n} events "
                          f"from {addr}")

    def run(self):
        try:
            sys.stdout.reconfigure(line_buffering=True)
        except AttributeError:
            pass
        threading.Thread(target=self.run_mapper, daemon=True).start()
        threading.Thread(target=self.run_streamer, daemon=True).start()
        self.run_ingress()


def main():
    bind_host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    mapper_port = int(sys.argv[2]) if len(sys.argv) > 2 else 9999
    media_port = int(sys.argv[3]) if len(sys.argv) > 3 else 6000
    width = int(sys.argv[4]) if len(sys.argv) > 4 else 1024
    height = int(sys.argv[5]) if len(sys.argv) > 5 else 768
    sim = MECSim(bind_host, mapper_port, media_port, width, height)
    try:
        sim.run()
    except KeyboardInterrupt:
        pass
    finally:
        sim.running = False
        if sim.source.proc:
            sim.source.proc.terminate()
        print("\n[sim] stopped")


if __name__ == "__main__":
    main()
