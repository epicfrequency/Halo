#!/usr/bin/env python3
"""
race_stress.py — hammers the daemon with the message mix that provoked the
cross-thread ALSA bug, so ThreadSanitizer has something to catch.

The failure this exists for: FLUSH is handled on the network thread and calls
snd_pcm_drop(), while the writer thread is inside snd_pcm_writei(). drop()
leaves the stream in SETUP and the next write returns -EBADFD, which the
writer treats as fatal — so on real hardware every seek and every stop had a
chance of killing playback outright. A hard-cut FORMAT is the same shape,
one step worse: it can close the handle the writer is mid-write on.

Neither is reliably reproducible by playing music and hoping. This drives
audio continuously and interleaves FLUSH and FORMAT at a much higher rate
than a person could, which under TSan is enough to surface the unsynchronised
access even on runs where the timing happens not to break anything.

Usage: race_stress.py [port] [seconds]
Exits non-zero if the daemon stops answering — i.e. the stream died.
"""
import socket
import struct
import sys
import threading
import time

HALO_MAGIC = 0x484C4F31
HEADER = "<IHHIIQ"


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5599
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    seq = [0]
    send_lock = threading.Lock()

    def send(msg_type, payload=b""):
        with send_lock:
            seq[0] += 1
            s.sendall(struct.pack(HALO_MAGIC.__class__ and HEADER, HALO_MAGIC,
                                  msg_type, 0, len(payload), 0, seq[0]) + payload)

    send(0x01, struct.pack("<I", 1))  # HELLO

    stop = threading.Event()
    format_id = [1]
    errors = []

    def announce(fid, preannounce=0):
        # 2ch/16-bit/44.1k PCM: the shape does not matter here, only that the
        # daemon opens, writes and tears down while another thread interferes.
        send(0x03, struct.pack("<IIBBBBIB", fid, 44100, 16, 2, 0, 0, 0x3,
                               preannounce) + b"\0\0\0")

    announce(format_id[0])

    def audio_feeder():
        chunk = struct.pack("<I", format_id[0]) + b"\x01\x02" * 4096
        while not stop.is_set():
            try:
                send(0x04, struct.pack("<I", format_id[0]) + chunk[4:])
            except OSError as error:
                errors.append(f"audio send failed: {error}")
                return
            time.sleep(0.002)

    def disruptor():
        """FLUSH and hard-cut FORMAT, the two things that touch the device
        from the network thread while the writer is using it."""
        while not stop.is_set():
            try:
                send(0x05)  # FLUSH — snd_pcm_drop + prepare
                time.sleep(0.007)
                format_id[0] += 1
                announce(format_id[0])  # hard cut — may reopen the device
                time.sleep(0.011)
            except OSError as error:
                errors.append(f"control send failed: {error}")
                return

    def drain():
        s.settimeout(1.0)
        while not stop.is_set():
            try:
                s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return

    threads = [threading.Thread(target=fn, daemon=True)
               for fn in (audio_feeder, disruptor, drain)]
    for thread in threads:
        thread.start()
    time.sleep(duration)
    stop.set()
    for thread in threads:
        thread.join(timeout=2)

    if errors:
        print("FAIL  " + "; ".join(errors))
        return 1

    # Still alive? A PING must still come back — the writer thread declaring
    # a fatal ALSA error would have closed the stream, not the socket, so
    # liveness alone is not proof; the caller checks the log for that.
    try:
        send(0x11)
        s.settimeout(3)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            header = s.recv(24)
            if not header:
                break
            if len(header) >= 6 and struct.unpack("<H", header[4:6])[0] == 0x12:
                print("PASS  daemon survived the FLUSH/FORMAT storm")
                return 0
        print("FAIL  no PONG after the storm — daemon stopped responding")
        return 1
    except OSError as error:
        print(f"FAIL  daemon stopped responding: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
