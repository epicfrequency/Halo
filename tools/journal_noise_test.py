#!/usr/bin/env python3
"""
journal_noise_test.py — the status line must not flood the journal.

The daemon prints its status line "on change, plus a heartbeat", which is the
right design and was not what the code did. The line carried the elapsed time
and the ring percentage, and both move several times a second, so the
change filter matched every time: a real twelve-hour journal held 27,426
lines, 23,649 of them this one status line. On a Pi booting off an SD card
that is the daemon's largest single source of writes, and it buries the lines
somebody is actually reading — the format switches, the rejects, the xruns.

So this streams steadily for a while and counts what reaches stderr. Playing
normally is exactly the case that used to be loudest: nothing interesting is
happening, which is precisely when the log should be quiet.

Usage: journal_noise_test.py <port> <daemon-log-path> [seconds]
"""
import re
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

# One line for the format opening, and one heartbeat's worth of slack. The
# pre-fix code produced roughly three a second, so anything near the old
# behaviour clears this by two orders of magnitude and the exact bound here
# is not delicate.
MAX_STATUS_LINES = 4


def main():
    port = int(sys.argv[1])
    log_path = sys.argv[2]
    duration = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0

    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    seq = [0]

    def send(msg_type, payload=b""):
        seq[0] += 1
        sock.sendall(struct.pack(HEADER, HALO_MAGIC, msg_type, 0,
                                 len(payload), 0, seq[0]) + payload)

    def recv_message():
        head = b""
        while len(head) < 24:
            chunk = sock.recv(24 - len(head))
            if not chunk:
                raise SystemExit("daemon closed the connection")
            head += chunk
        _, msg_type, _, length, _, _ = struct.unpack(HEADER, head)
        body = b""
        while len(body) < length:
            body += sock.recv(length - len(body))
        return msg_type, body

    send(MSG_HELLO, struct.pack("<I", 1))
    while recv_message()[0] != MSG_CAPS:
        pass

    fmt_id = 1
    send(MSG_FORMAT, struct.pack("<IIBBBBIB", fmt_id, RATE, 32,
                                 CHANNELS, 0, 0, 0x3, 0) + b"\0\0\0")

    # Real time, real pacing. Blasting the audio in as fast as the socket
    # takes it would fill the ring and change the very percentage this is
    # meant to prove stays out of the log.
    started = time.monotonic()
    sent_frames = 0
    while time.monotonic() - started < duration:
        target = int((time.monotonic() - started) * RATE)
        if target > sent_frames:
            frames = target - sent_frames
            payload = struct.pack("<I", fmt_id)
            payload += b"\0" * (frames * BYTES_PER_FRAME)
            send(MSG_AUDIO_DATA, payload)
            sent_frames = target
        time.sleep(0.02)

    sock.close()
    time.sleep(0.5)

    with open(log_path, "r", errors="replace") as handle:
        lines = handle.read().splitlines()
    status = [line for line in lines if re.search(r"\[halo\] ", line)]

    seconds = time.monotonic() - started
    print(f"  {len(status)} status line(s) in {seconds:.1f}s of steady playback")
    if len(status) > MAX_STATUS_LINES:
        for line in status[:6]:
            print(f"    {line}")
        print(f"  FAIL  status line floods the journal "
              f"({len(status)} lines, limit {MAX_STATUS_LINES})")
        return 1
    print("  PASS  journal stays quiet while nothing changes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
