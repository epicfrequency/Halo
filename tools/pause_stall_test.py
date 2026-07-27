#!/usr/bin/env python3
"""
pause_stall_test.py — the paused-and-overfull deadlock, as a regression test.

This is the failure it pins down: while paused the daemon's writer thread is
stopped, so its ring never drains. A sender that keeps pushing audio then
fills the ring, and the reader thread blocks trying to place the next bytes.
That reader is also what delivers PAUSE/RESUME/FLUSH — control and audio
share one socket — so the RESUME that would restart the writer can never
arrive. Unlike the gapless variant there is no timeout that escapes it; the
daemon stays wedged until it is restarted.

It is deterministic rather than racy, which is why it reproduced on every
single pause at DSD512 rates: the sender's lookahead in bytes simply exceeds
the receiver's buffer, so the ring fills every time.

Pausing first is also what makes this testable against the ALSA stub. The
stub consumes instantly, so a running stream's ring never fills no matter how
hard the test pushes — but a *paused* one fills regardless of how fast the
device is, because nothing is consuming at all.

Usage: pause_stall_test.py [port]
"""
import socket
import struct
import sys
import time

HALO_MAGIC = 0x484C4F31
HEADER = "<IHHIIQ"

MSG_HELLO, MSG_CAPS = 0x01, 0x02
MSG_FORMAT, MSG_AUDIO_DATA = 0x03, 0x04
MSG_PAUSE, MSG_RESUME = 0x07, 0x08
MSG_PING, MSG_PONG = 0x11, 0x12

# Comfortably past the daemon's per-ring capacity, so the ring is certain to
# be full and the old code certain to be blocked in it.
OVERFILL_BYTES = 24 << 20

failures = []


def check(label, ok, detail=""):
    if ok:
        print(f"  PASS  {label}")
    else:
        failures.append(label)
        print(f"  FAIL  {label}{(' — ' + detail) if detail else ''}")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5651
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    seq = [0]

    def send(msg_type, payload=b""):
        seq[0] += 1
        sock.sendall(struct.pack(HEADER, HALO_MAGIC, msg_type, 0,
                                 len(payload), 0, seq[0]) + payload)

    def recv_msg(timeout):
        sock.settimeout(timeout)
        header = b""
        while len(header) < 24:
            chunk = sock.recv(24 - len(header))
            if not chunk:
                raise ConnectionError("peer closed")
            header += chunk
        _magic, mtype, _flags, length, _res, _seq = struct.unpack(HEADER, header)
        body = b""
        while len(body) < length:
            chunk = sock.recv(length - len(body))
            if not chunk:
                raise ConnectionError("peer closed")
            body += chunk
        return mtype, body

    send(MSG_HELLO, struct.pack("<I", 1))
    mtype, _ = recv_msg(5)
    check("CAPS returned", mtype == MSG_CAPS, f"got 0x{mtype:02x}")

    # PCM 16/44.1 stereo — the format is irrelevant, only the byte volume is.
    send(MSG_FORMAT, struct.pack("<IIBBBBIB", 1, 44100, 16, 2, 0, 0, 0x3, 0) + b"\0\0\0")
    time.sleep(0.3)

    send(MSG_PAUSE)
    time.sleep(0.2)

    print(f"overfilling the paused stream with {OVERFILL_BYTES >> 20} MiB")
    chunk = struct.pack("<I", 1) + bytes(64 << 10)
    sent = 0
    # A send timeout here is itself the bug: it means the daemon stopped
    # reading its socket, which is exactly the state that swallows RESUME.
    sock.settimeout(20)
    try:
        while sent < OVERFILL_BYTES:
            send(MSG_AUDIO_DATA, chunk)
            sent += len(chunk)
    except (socket.timeout, OSError) as error:
        check("daemon kept reading while paused and overfull", False, str(error))
        print("\n1 check(s) FAILED")
        return 1
    check("daemon kept reading while paused and overfull", True)

    # The real question: is the control path still alive?
    send(MSG_PING)
    got_pong = False
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            mtype, _ = recv_msg(max(deadline - time.monotonic(), 0.1))
        except (socket.timeout, ConnectionError):
            break
        if mtype == MSG_PONG:
            got_pong = True
            break
    check("PING answered while paused and overfull", got_pong,
          "control path is blocked behind buffered audio")

    send(MSG_RESUME)
    time.sleep(0.5)
    send(MSG_PING)
    resumed = False
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            mtype, _ = recv_msg(max(deadline - time.monotonic(), 0.1))
        except (socket.timeout, ConnectionError):
            break
        if mtype == MSG_PONG:
            resumed = True
            break
    check("RESUME got through and daemon still responds", resumed)

    sock.close()
    if failures:
        print(f"\n{len(failures)} check(s) FAILED: " + ", ".join(failures))
        return 1
    print("\nall pause-stall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
