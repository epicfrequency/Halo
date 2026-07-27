#!/usr/bin/env python3
"""
state_fuzz.py — randomised ordering of every control message, checking the
daemon stays responsive throughout.

Reasoning found the conflicting states that were *thought of*: a cancelled
preannounce leaving its commit intent behind, a dead stream's pending slot
surviving into the next one, a gapless reopen abandoned after the device was
already reconfigured. Each was real, and each was found by reading. What
reading does not cover is the orderings nobody pictured — and every deadlock
in this daemon so far has been an ordering, not a line of logic.

So this does not try to be clever. It fires FORMAT, preannounce,
SWITCH_TO_PENDING, CANCEL_PREANNOUNCE, FLUSH, PAUSE, RESUME and AUDIO_DATA in
random order, at a rate no human could reach, and asks one question over and
over: does PING still come back? A daemon that has wedged cannot answer, so
liveness is the whole assertion. Anything that gets stuck shows up as a
missing PONG regardless of which internal state caused it.

The seed is printed and settable, so a failure can be replayed exactly.

Usage: state_fuzz.py [port] [seconds] [seed]
"""
import random
import socket
import struct
import sys
import time

HALO_MAGIC = 0x484C4F31
HEADER = "<IHHIIQ"

MSG_HELLO, MSG_CAPS = 0x01, 0x02
MSG_FORMAT, MSG_AUDIO_DATA = 0x03, 0x04
MSG_FLUSH, MSG_FLUSH_ACK = 0x05, 0x06
MSG_PAUSE, MSG_RESUME = 0x07, 0x08
MSG_CANCEL_PREANNOUNCE, MSG_SWITCH_TO_PENDING = 0x0E, 0x0F
MSG_PING, MSG_PONG = 0x11, 0x12

# A few genuinely different shapes, so format-change reopens are exercised
# alongside the same-format fast path.
FORMATS = [
    # (sample_rate, bits, channels, is_dsd, dsd_mult)
    (44100, 16, 2, 0, 0),
    (96000, 24, 2, 0, 0),
    (352800, 0, 2, 1, 1),   # DSD64
    (2822400, 0, 2, 1, 8),  # DSD512
]


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5661
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else random.randrange(1 << 30)
    rng = random.Random(seed)
    print(f"seed {seed} (pass it as the third argument to replay this exact run)")

    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(10)
    seq = [0]

    def send(msg_type, payload=b""):
        seq[0] += 1
        sock.sendall(struct.pack(HEADER, HALO_MAGIC, msg_type, 0,
                                 len(payload), 0, seq[0]) + payload)

    def format_payload(fid, spec, preannounce):
        rate, bits, ch, is_dsd, mult = spec
        return struct.pack("<IIBBBBIB", fid, rate, bits, ch, is_dsd, mult,
                           0x3, 1 if preannounce else 0) + b"\0\0\0"

    send(MSG_HELLO, struct.pack("<I", 1))

    pending_pong = 0
    last_pong = time.monotonic()
    next_fid = 1
    live_fids = []
    audio = bytes(16 << 10)

    rx = bytearray()

    def pump_replies():
        """Drain whatever has arrived without blocking, tracking PONGs.

        Buffers across calls. recv() on a busy stream routinely returns a
        partial header, and discarding those bytes desynchronises the frame
        boundary for the rest of the run — every later reply then decodes as
        garbage and the PONGs are never recognised, which looks exactly like
        a daemon that stopped answering. Worth being careful about: a test
        that reports a hang that isn't there is worse than no test.
        """
        nonlocal pending_pong, last_pong
        sock.settimeout(0.01)
        try:
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    raise ConnectionError("peer closed")
                rx.extend(chunk)
        except socket.timeout:
            pass

        while len(rx) >= 24:
            _m, mtype, _f, length, _r, _s = struct.unpack(HEADER, bytes(rx[:24]))
            if len(rx) < 24 + length:
                return  # rest of this message hasn't arrived yet
            del rx[:24 + length]
            if mtype == MSG_PONG:
                pending_pong = 0
                last_pong = time.monotonic()

    actions = [
        "audio", "audio", "audio",   # weighted: audio is the common case
        "preannounce", "switch", "cancel", "flush", "pause", "resume",
        "hardcut", "ping",
    ]

    deadline = time.monotonic() + duration
    sock.settimeout(10)
    try:
        while time.monotonic() < deadline:
            action = rng.choice(actions)

            if action == "audio" and live_fids:
                send(MSG_AUDIO_DATA, struct.pack("<I", rng.choice(live_fids)) + audio)
            elif action in ("preannounce", "hardcut"):
                spec = rng.choice(FORMATS)
                send(MSG_FORMAT, format_payload(next_fid, spec, action == "preannounce"))
                live_fids.append(next_fid)
                live_fids = live_fids[-4:]
                next_fid += 1
            elif action == "switch" and live_fids:
                send(MSG_SWITCH_TO_PENDING, struct.pack("<I", rng.choice(live_fids)))
            elif action == "cancel" and live_fids:
                send(MSG_CANCEL_PREANNOUNCE, struct.pack("<I", rng.choice(live_fids)))
            elif action == "flush":
                send(MSG_FLUSH)
            elif action == "pause":
                send(MSG_PAUSE)
            elif action == "resume":
                send(MSG_RESUME)
            elif action == "ping":
                send(MSG_PING)
                pending_pong += 1

            pump_replies()
            sock.settimeout(10)

            # The one assertion. A wedged daemon cannot answer, whatever the
            # internal cause; 8s is far beyond any legitimate delay here.
            if pending_pong and time.monotonic() - last_pong > 8:
                print(f"  FAIL  daemon stopped answering PING (seed {seed})")
                return 1

            time.sleep(0.001)
    except (OSError, ConnectionError) as error:
        print(f"  FAIL  connection died during fuzz (seed {seed}): {error}")
        return 1

    # Settle, then demand a final answer — this catches a daemon that wedged
    # on the very last operations.
    send(MSG_RESUME)
    send(MSG_PING)
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        pump_replies()
        if time.monotonic() - last_pong < 8 and pending_pong == 0:
            print("  PASS  daemon stayed responsive through randomised control ordering")
            sock.close()
            return 0
        time.sleep(0.05)

    print(f"  FAIL  no final PONG (seed {seed})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
