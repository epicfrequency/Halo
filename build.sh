#!/usr/bin/env bash
# build.sh — build halo-daemon on the Pi5 (or any Linux host with ALSA).
set -euo pipefail
cd "$(dirname "$0")"

need() { command -v "$1" >/dev/null 2>&1; }

echo "==> Checking build dependencies"
missing=()
need gcc  || missing+=("build-essential")
need make || missing+=("build-essential")
# The header, not just the runtime lib — a Pi with sound working still
# usually lacks the -dev package.
[ -f /usr/include/alsa/asoundlib.h ] || missing+=("libasound2-dev")

if [ ${#missing[@]} -gt 0 ]; then
    # Deduplicate (build-essential can be added twice above). Plain read
    # loop instead of `mapfile`, which needs bash 4+.
    uniq_missing=()
    while IFS= read -r m; do uniq_missing+=("$m"); done < <(printf '%s\n' "${missing[@]}" | sort -u)
    missing=("${uniq_missing[@]}")
    echo "Missing: ${missing[*]}"
    echo
    echo "Install with:"
    echo "    apt install ${missing[*]}          # Debian / Raspberry Pi OS"
    echo "    dietpi-software install 17        # DietPi: 'Build-Essentials'"
    echo "                                      # (libasound2-dev still via apt)"
    exit 1
fi
echo "    gcc, make, alsa headers present"

# Which ALSA DSD format to compile for is a property of the DAC, and getting
# it wrong yields noise rather than an error — so detect it rather than
# leaving it to a default. USB Audio Class devices advertise it in
# /proc/asound/cardN/stream0; the first DSD-capable card wins, which is the
# right answer for the single-DAC case and is reported either way so a
# multi-DAC setup can override with `make DSD_FORMAT=...`.
detect_dsd_format() {
    local f
    [ -d /proc/asound ] || return 0
    for f in /proc/asound/card[0-9]*/stream0; do
        [ -f "$f" ] || continue
        # e.g. "Format: SPECIAL DSD_U32_BE"
        local fmt
        fmt=$(grep -oE 'DSD_U(8|16_LE|16_BE|32_LE|32_BE)' "$f" | head -1) || true
        if [ -n "$fmt" ]; then printf '%s\n' "$fmt"; return 0; fi
    done
}

DSD_FORMAT="$(detect_dsd_format || true)"
if [ -n "$DSD_FORMAT" ]; then
    echo "==> Detected DSD format: $DSD_FORMAT"
else
    echo "==> No DSD-capable device detected; building with the default (DSD_U32_LE)."
    echo "    Harmless if you only play PCM. If a DSD DAC is attached but off,"
    echo "    power it on and re-run so the format can be detected."
fi

echo "==> Building"
make clean >/dev/null 2>&1 || true
if [ -n "$DSD_FORMAT" ]; then
    make DSD_FORMAT="$DSD_FORMAT"
else
    make
fi

if [ ! -x ./halo-daemon ]; then
    echo "Build reported success but ./halo-daemon is missing" >&2
    exit 1
fi

echo
echo "==> Built: $(pwd)/halo-daemon"
echo
echo "Next: pick your DAC and install as a service:"
echo "    aplay -l                 # find your card/device numbers"
echo "    sudo ./install.sh        # interactive, or: sudo ./install.sh --device hw:1,0 --port 5555"
