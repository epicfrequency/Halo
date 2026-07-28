#!/usr/bin/env python3
"""
prime_gate_test.py — playback must not start on a sliver of audio.

After a seek the sender re-seeks its decoder before it can send anything, and
on real DSD material that took 250-600ms. The device, meanwhile, had been
dropped and prepared. Starting it on the first few milliseconds to arrive
meant it played them, starved, and logged an -EPIPE plus a dropout report —
on essentially every seek.

An unstarted device cannot underrun, so the writer waits for a cushion in the
ring before it hands ALSA anything. This checks that it does: a sliver of
audio must sit unplayed, and the same stream must start once enough arrives.

Waiting is only correct while more audio might still be coming, so the second
half checks the escape hatch too — a stream that ends holding less than the
cushion has to play out rather than stall forever.

Usage: prime_gate_test.py <port> <runtime-dir>
"""
import json
import os
import socket
import struct
import sys
import time

HALO_MAGIC = 0x484C4F31
HEADER = "<IHHIIQ"

MSG_HELLO, MSG_CAPS = 0x01, 0x02
MSG_FORMAT, MSG_AUDIO_DATA = 0x03, 0x04

RATE = 44100
CHANNELS = 2
BYTES_PER_FRAME = CHANNELS * 4  # S32

failures = []


def check(label, ok, detail=""):
    if ok:
        print(f"  PASS  {label}")
    else:
        failures.append(label)
        print(f"  FAIL  {label}{(' — ' + detail) if detail else ''}")


def position_frames(runtime_dir):
    path = os.path.join(runtime_dir, "status.json")
    try:
        with open(path) as handle:
            return json.load(handle).get("position_frames", 0)
    except (OSError, ValueError):
        return 0


def main():
    port = int(sys.argv[1])
    runtime_dir = sys.argv[2]

    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    seq = [0]

    def send(msg_type, payload=b""):
        seq[0] += 1
        sock.sendall(struct.pack(HEADER, HALO_MAGIC, msg_type, 0,
                                 len(payload), 0, seq[0]) + payload)

    def audio(fmt_id, frames):
        send(MSG_AUDIO_DATA,
             struct.pack("<I", fmt_id) + b"\0" * (frames * BYTES_PER_FRAME))

    send(MSG_HELLO, struct.pack("<I", 1))
    head = b""
    while len(head) < 24:
        head += sock.recv(24 - len(head))
    _, _, _, length, _, _ = struct.unpack(HEADER, head)
    body = b""
    while len(body) < length:
        body += sock.recv(length - len(body))

    # 20ms — enough to start playback under the old behaviour, nowhere near
    # enough to survive the gap that follows it.
    send(MSG_FORMAT, struct.pack("<IIBBBBIB", 1, RATE, 32,
                                 CHANNELS, 0, 0, 0x3, 0) + b"\0\0\0")
    audio(1, RATE // 50)
    time.sleep(0.5)
    check("a 20ms sliver does not start playback",
          position_frames(runtime_dir) == 0,
          f"position_frames={position_frames(runtime_dir)}")

    # Now the burst the sender produces once its decoder is ready.
    audio(1, RATE)
    time.sleep(0.5)
    started = position_frames(runtime_dir)
    check("playback starts once the cushion is there", started > 0,
          f"position_frames={started}")

    # A stream that ends below the cushion must still play out. The daemon's
    # hatch is two seconds, so this deliberately waits longer than the check
    # above rather than assuming the gate opened for the same reason.
    send(MSG_FORMAT, struct.pack("<IIBBBBIB", 2, RATE, 32,
                                 CHANNELS, 0, 0, 0x3, 0) + b"\0\0\0")
    time.sleep(0.3)
    before = position_frames(runtime_dir)
    audio(2, RATE // 50)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if position_frames(runtime_dir) > before:
            break
        time.sleep(0.1)
    check("a short final stream is not stranded by the gate",
          position_frames(runtime_dir) > before,
          f"position_frames stayed at {before}")

    sock.close()
    if failures:
        print(f"\n{len(failures)} prime-gate check(s) failed")
        return 1
    print("\nall prime-gate checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
