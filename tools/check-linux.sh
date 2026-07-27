#!/usr/bin/env bash
# check-linux.sh — build and self-test halo-daemon against the *real* ALSA
# headers, on the Pi5's own architecture, inside a container.
#
# `make check` (the bundled stub) proves the code is self-consistent. This
# proves it agrees with actual libasound: real header types and macros, real
# format constants, and the real snd_pcm_open failure path — which is also
# the only way to exercise FORMAT_REJECTED off-hardware, since a container
# has no sound card and every open genuinely fails.
#
# Still cannot test: whether a real DAC accepts native DSD, DSD bit order,
# or how it sounds. Those need the Pi.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="${HALO_CHECK_IMAGE:-arm64v8/debian:bookworm-slim}"
PORT="${HALO_CHECK_PORT:-5601}"

command -v docker >/dev/null || { echo "docker not found (try: brew install colima docker && colima start --arch aarch64)" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "docker engine not running (try: colima start --arch aarch64)" >&2; exit 1; }

# Containers can only mount paths the VM shares — colima shares $HOME by
# default, so copy there rather than assuming an arbitrary source path works.
WORK="$(mktemp -d "${HOME}/.halo-check-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp -R . "$WORK/"

echo "==> Building against real libasound in $IMAGE"
docker run --rm -v "$WORK":/src -w /src "$IMAGE" bash -c "
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential libasound2-dev python3 >/dev/null
  echo \"    gcc  \$(gcc -dumpversion) / arch \$(uname -m) / libasound \$(dpkg-query -f '\${Version}' -W libasound2-dev)\"
  make clean >/dev/null 2>&1 || true
  make CFLAGS='-O2 -Wall -Wextra -Werror -std=c11 -pthread'

  mkdir -p /run/halo-daemon
  # hw:99,0 does not exist anywhere, so every open fails — which is exactly
  # the FORMAT_REJECTED path, unreachable with the stub.
  ./halo-daemon hw:99,0 $PORT > /tmp/daemon.log 2>&1 &
  sleep 1

  python3 - <<'PYEOF'
import socket, struct, sys
M, H = 0x484C4F31, '<IHHIIQ'
def snd(s,q,t,p=b''):
    q[0]+=1; s.sendall(struct.pack(H,M,t,0,len(p),0,q[0])+p)
def rx(s,n):
    b=b''
    while len(b)<n:
        c=s.recv(n-len(b))
        if not c: raise ConnectionError
        b+=c
    return b
def rcv(s):
    _m,t,_f,l,_r,_q = struct.unpack(H, rx(s,24)); return t,(rx(s,l) if l else b'')

fails=[]
def check(label, ok):
    print(f\"  {'PASS' if ok else 'FAIL'}  {label}\")
    if not ok: fails.append(label)

s=socket.create_connection(('127.0.0.1', $PORT), timeout=5); q=[0]
snd(s,q,0x01,struct.pack('<I',1))
t,p=rcv(s)
check('CAPS returned', t==0x02)
check('CAPS length-extended', len(p)>=20)

snd(s,q,0x03,struct.pack('<IIBBBBIB',1,44100,16,2,0,0,0x3,0)+b'\x00\x00\x00')
s.settimeout(3)
got_reject=False
try:
    t,p=rcv(s)
    if t==0x10:
        fid,reason=struct.unpack('<II',p[:8])
        got_reject=(fid==1)
        print(f'         format_id={fid} reason={reason}')
except socket.timeout:
    pass
check('unopenable format answered with FORMAT_REJECTED', got_reject)
snd(s,q,0x0D); s.close()
sys.exit(1 if fails else 0)
PYEOF
  status=\$?
  if [ \$status -ne 0 ]; then echo; echo '--- daemon log ---'; cat /tmp/daemon.log; fi
  exit \$status
"
echo "==> real-ALSA check passed"
