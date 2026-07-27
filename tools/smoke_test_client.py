#!/usr/bin/env python3
"""
smoke_test_client.py — minimal protocol-level exerciser for halo-daemon.

This is NOT a macOS reference sender. It's a throwaway test harness used to
verify the daemon's framing/parsing/threading survives a real TCP round
trip (handshake, format announce, audio bytes, flush, pause/resume, bye)
without a real ALSA card attached — useful in CI or a sandbox that has no
sound hardware. On real Pi5 hardware with a DAC attached, run this against
`hw:X,Y` and watch it actually open the device / report real CAPS.
"""
import socket
import struct
import sys
import time

HALO_MAGIC = 0x484C4F31

MSG_HELLO = 0x01
MSG_CAPS = 0x02
MSG_FORMAT = 0x03
MSG_AUDIO_DATA = 0x04
MSG_FLUSH = 0x05
MSG_FLUSH_ACK = 0x06
MSG_PAUSE = 0x07
MSG_RESUME = 0x08
MSG_POSITION = 0x09
MSG_UNDERRUN = 0x0A
MSG_BYE = 0x0D

seq = [0]


def send_msg(sock, msg_type, payload=b""):
    seq[0] += 1
    # <IHHIIQ: magic, type, flags, length, _reserved, seq — 24 bytes total,
    # matching struct halo_header in src/protocol.h (the _reserved uint32
    # exists purely to put `seq` on an 8-byte boundary).
    hdr = struct.pack("<IHHIIQ", HALO_MAGIC, msg_type, 0, len(payload), 0, seq[0])
    sock.sendall(hdr + payload)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return buf


def recv_msg(sock, timeout=2.0):
    sock.settimeout(timeout)
    hdr = recv_exact(sock, 24)
    magic, mtype, flags, length, _reserved, s = struct.unpack("<IHHIIQ", hdr)
    assert magic == HALO_MAGIC, f"bad magic 0x{magic:x}"
    payload = recv_exact(sock, length) if length else b""
    return mtype, payload


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5555

    s = socket.create_connection((host, port), timeout=3)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"connected to {host}:{port}")

    # 1. HELLO -> expect CAPS
    send_msg(s, MSG_HELLO, struct.pack("<I", 1))
    mtype, payload = recv_msg(s)
    assert mtype == MSG_CAPS, f"expected CAPS, got 0x{mtype:x}"
    rate, bits, chans, native_dsd, dop, _pad, dsd_mask = struct.unpack("<IIBBBBI", payload)
    print(f"CAPS: max_rate={rate} max_bits={bits} max_ch={chans} "
          f"native_dsd={native_dsd} dop={dop} dsd_mask=0x{dsd_mask:x}")

    # 2. FORMAT (initial, non-preannounce): 44.1kHz/16bit/2ch PCM
    fmt = struct.pack("<IIBBBBIBBBB",
                       1,          # format_id
                       44100,      # sample_rate
                       16,         # bits_per_sample
                       2,          # channels
                       0,          # is_dsd = PCM
                       0,          # dsd_rate_mult
                       0x3,        # channel_mask (FL|FR)
                       0,          # is_preannounce
                       0, 0, 0)    # padding
    send_msg(s, MSG_FORMAT, fmt)
    print("sent initial FORMAT (44.1kHz/16/2ch PCM)")
    time.sleep(0.2)

    # 3. AUDIO_DATA: format_id=1 header + 4 silent frames (16 bytes)
    audio_hdr = struct.pack("<I", 1)
    silence = b"\x00" * 16
    send_msg(s, MSG_AUDIO_DATA, audio_hdr + silence)
    print("sent AUDIO_DATA (16 bytes of silence)")

    # 4. Try to read a POSITION update (only arrives if stream_open — will
    # NOT arrive in a sandbox with no real ALSA card, since initial open
    # fails and stream_open stays 0; that's expected here, not a bug.)
    try:
        mtype, payload = recv_msg(s, timeout=1.0)
        print(f"got message 0x{mtype:x} ({len(payload)} bytes)")
    except socket.timeout:
        print("no POSITION within 1s (expected if no real ALSA card is attached)")

    # 5. FLUSH -> expect FLUSH_ACK
    send_msg(s, MSG_FLUSH)
    mtype, payload = recv_msg(s)
    assert mtype == MSG_FLUSH_ACK, f"expected FLUSH_ACK, got 0x{mtype:x}"
    print("FLUSH_ACK received")

    # 6. PAUSE / RESUME (no reply expected, just verify daemon doesn't choke)
    send_msg(s, MSG_PAUSE)
    time.sleep(0.05)
    send_msg(s, MSG_RESUME)
    print("PAUSE/RESUME sent, no crash")

    # 7. BYE
    send_msg(s, MSG_BYE)
    print("BYE sent, closing")
    s.close()
    print("SMOKE TEST PASSED")


if __name__ == "__main__":
    main()
