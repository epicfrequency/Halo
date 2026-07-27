#!/usr/bin/env python3
"""Drives a running halo-daemon through every message in PROTOCOL.md and
asserts the replies and runtime files. Used by `make check` against the
ALSA stub, so protocol regressions surface locally in seconds.

Exits non-zero on the first failed assertion.
"""
import hashlib, json, os, socket, struct, sys, time

MAGIC = 0x484C4F31
HDR = "<IHHIIQ"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5601
RUNTIME = sys.argv[2] if len(sys.argv) > 2 else "/tmp/halo-selftest"

failures = []
def check(label, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' — ' + detail) if detail and not ok else ''}")
    if not ok:
        failures.append(label)

def send(s, q, t, p=b""):
    q[0] += 1
    s.sendall(struct.pack(HDR, MAGIC, t, 0, len(p), 0, q[0]) + p)

def rx(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise ConnectionError("daemon closed the connection")
        b += c
    return b

def recv(s):
    magic, t, _f, ln, _r, _q = struct.unpack(HDR, rx(s, 24))
    if magic != MAGIC:
        raise ValueError("bad magic — framing is broken")
    return t, (rx(s, ln) if ln else b"")

s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
q = [0]

print("handshake")
send(s, q, 0x01, struct.pack("<I", 1))
t, p = recv(s)
check("CAPS returned", t == 0x02, f"got 0x{t:02x}")
check("CAPS is length-extended (>=20B)", len(p) >= 20, f"{len(p)} bytes")
flags = struct.unpack("<I", p[16:20])[0] if len(p) >= 20 else 0
check("advertises FORMAT_REJECTED|PING|SKIPS_UNKNOWN", flags & 0x7 == 0x7, hex(flags))

print("stream")
send(s, q, 0x03, struct.pack("<IIBBBBIB", 1, 44100, 16, 2, 0, 0, 0x3, 0) + b"\0\0\0")
send(s, q, 0x04, struct.pack("<I", 1) + b"\0" * 8192)

print("metadata / cover art")
meta = json.dumps({
    "track": {"title": "So What", "artist": "Miles Davis"},
    "album": {"title": "Kind of Blue", "tracks": [{"track_number": 1, "title": "So What"}]},
    "source": {"kind": "dsd", "dsd_rate_label": "DSD64", "converted_to_pcm": True},
}).encode()
send(s, q, 0x0B, meta)
img = b"\xff\xd8\xff\xe0" + b"FAKEJPEG" * 500
digest = hashlib.sha256(img).digest()
send(s, q, 0x0C, digest + struct.pack("<I", len(img)) + img)
send(s, q, 0x0C, digest + struct.pack("<I", len(img)) + img)  # identical: must dedupe

print("liveness / forward compatibility / gapless / seek")
send(s, q, 0x11)                                   # PING
send(s, q, 0x7F, b"\x01\x02\x03\x04")              # unknown type: must be skipped
send(s, q, 0x03, struct.pack("<IIBBBBIB", 2, 44100, 16, 2, 0, 0, 0x3, 1) + b"\0\0\0")
send(s, q, 0x04, struct.pack("<I", 2) + b"\0" * 4096)
send(s, q, 0x0F, struct.pack("<I", 2))             # SWITCH_TO_PENDING
send(s, q, 0x05)                                   # FLUSH

seen = set()
s.settimeout(3)
try:
    for _ in range(40):
        t, _p = recv(s)
        seen.add(t)
except (socket.timeout, ConnectionError):
    pass

print("replies")
check("survived the unknown message type", bool(seen), "connection died")
check("FLUSH_ACK received", 0x06 in seen)
check("POSITION received", 0x09 in seen)
check("PONG received", 0x12 in seen)

print("runtime files")
time.sleep(0.5)
for name in ("caps.json", "status.json", "metadata.json", "coverart.bin", "coverart.sha256"):
    check(f"{name} exists", os.path.exists(os.path.join(RUNTIME, name)))

try:
    d = json.load(open(os.path.join(RUNTIME, "metadata.json")))
    check("metadata stored verbatim", d["track"]["title"] == "So What")
    check("album track list preserved", len(d["album"]["tracks"]) == 1)
    check("source block preserved", d["source"]["dsd_rate_label"] == "DSD64")
    st = json.load(open(os.path.join(RUNTIME, "status.json")))
    check("status.json is valid JSON with a format", "format" in st)
    caps = json.load(open(os.path.join(RUNTIME, "caps.json")))
    check("caps.json reports the device", "device" in caps)
    blob = open(os.path.join(RUNTIME, "coverart.bin"), "rb").read()
    stored = open(os.path.join(RUNTIME, "coverart.sha256")).read().strip()
    check("cover art bytes intact", blob == img)
    check("cover art digest matches", hashlib.sha256(blob).hexdigest() == stored)
except Exception as exc:
    check("runtime files parse", False, str(exc))

send(s, q, 0x0D)
s.close()

print()
if failures:
    print(f"{len(failures)} check(s) FAILED: {', '.join(failures)}")
    sys.exit(1)
print("all checks passed")
