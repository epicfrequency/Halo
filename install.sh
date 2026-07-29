#!/usr/bin/env bash
# install.sh — one step from a fresh machine to a running endpoint: installs
# what's missing, builds, picks the DAC, and registers the service. Safe to
# re-run to upgrade an existing install.
#
# There is deliberately no separate build script and no build-time tuning.
# The DSD packing a DAC wants (BE/LE, 8/16/32-bit) is discovered at runtime,
# so one binary drives any device — which is what makes it sane to build this
# on one machine and copy it to another.
#
# The one thing this exists to get right: the ALSA device and the TCP port
# appear in *two* files (the systemd unit and the Avahi service). Editing
# them by hand and forgetting one is the classic way to end up with a
# daemon the sender can discover but not reach, or vice versa. Both are
# generated here from the same two values.
set -euo pipefail
cd "$(dirname "$0")"

DEVICE=""
PORT="5555"
ASSUME_YES=0
SKIP_DEPS=0

usage() {
    cat <<USAGE
Usage: sudo ./install.sh [--device hw:CARD,DEV] [--port N] [--yes] [--skip-deps]

  --device   ALSA device, e.g. hw:1,0. Omit to choose interactively.
             Use hw: — not plughw:/default: — or ALSA silently converts
             formats and bit-perfect output is gone.
  --port     TCP port to listen on (default 5555).
  --yes      Don't prompt; requires --device.
  --skip-deps  Fail instead of installing missing packages via apt.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --device) DEVICE="${2:-}"; shift 2 ;;
        --port)   PORT="${2:-}";   shift 2 ;;
        --yes)    ASSUME_YES=1;    shift ;;
        --skip-deps) SKIP_DEPS=1;  shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

# Enumerate every ALSA playback device as "hw:C,D<TAB>label".
# Reads /proc/asound directly rather than parsing `aplay -l` output: the
# proc layout is stable and machine-readable, whereas aplay's text has
# changed shape across alsa-utils versions. cardN/usbid exists only for USB
# cards, which cleanly separates a USB DAC from the Pi's onboard bcm2835
# headphone and vc4-hdmi outputs without knowing their names.
enumerate_playback_devices() {
    local cardpath card name pcm dev bus dsd
    [ -d /proc/asound ] || return 0
    for cardpath in /proc/asound/card[0-9]*; do
        [ -d "$cardpath" ] || continue
        card="${cardpath##*/card}"
        name="$(cat "$cardpath/id" 2>/dev/null || echo "card $card")"
        if [ -e "$cardpath/usbid" ]; then bus="USB"; else bus="onboard"; fi
        # USB Audio Class devices list their altset formats here; a
        # DSD-capable DAC advertises DSD_U8/U16/U32. Advisory only — some
        # drivers don't populate it, and PCM-only setups are perfectly valid.
        if grep -qi 'DSD' "$cardpath/stream0" 2>/dev/null; then dsd=", DSD"; else dsd=""; fi
        # pcmNp = playback, pcmNc = capture; only playback is usable here.
        for pcm in "$cardpath"/pcm*p; do
            [ -d "$pcm" ] || continue
            dev="${pcm##*/pcm}"; dev="${dev%p}"
            printf 'hw:%s,%s\t%s (%s%s)\n' "$card" "$dev" "$name" "$bus" "$dsd"
        done
    done
}

[ "$(id -u)" -eq 0 ] || { echo "Must run as root: sudo ./install.sh (or directly, if already root)." >&2; exit 1; }
[[ "$PORT" =~ ^[0-9]+$ ]] || { echo "--port must be numeric, got '$PORT'" >&2; exit 1; }

# ------------------------------------------------------------ dependencies
# Installed rather than merely reported. A missing compiler or ALSA header is
# not a decision the person running this needs to be consulted about, and
# hand-copying an apt line out of an error message is the step where most
# first installs stall.
ensure_dependencies() {
    local missing=()
    command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || missing+=(build-essential)
    command -v make >/dev/null 2>&1 || missing+=(build-essential)
    [ -f /usr/include/alsa/asoundlib.h ] || missing+=(libasound2-dev)
    # Discovery is how the sender finds this machine without anyone typing an
    # IP address, so it counts as a dependency, not an optional extra.
    command -v avahi-daemon >/dev/null 2>&1 || missing+=(avahi-daemon)

    if [ ${#missing[@]} -eq 0 ]; then
        echo "==> Dependencies present"
        return 0
    fi

    # De-duplicate (build-essential can be added twice).
    local uniq=()
    local pkg
    for pkg in "${missing[@]}"; do
        case " ${uniq[*]-} " in *" $pkg "*) ;; *) uniq+=("$pkg") ;; esac
    done

    if [ "$SKIP_DEPS" -eq 1 ]; then
        echo "Missing: ${uniq[*]}" >&2
        echo "Re-run without --skip-deps, or install them yourself." >&2
        exit 1
    fi

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "Missing: ${uniq[*]}" >&2
        echo "No apt-get here — install the equivalents for your distribution:" >&2
        echo "  Fedora/RHEL:  dnf install gcc make alsa-lib-devel avahi" >&2
        echo "  Arch:         pacman -S base-devel alsa-lib avahi" >&2
        exit 1
    fi

    echo "==> Installing: ${uniq[*]}"
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${uniq[@]}"
}

ensure_dependencies

# ---------------------------------------------------------------- build
echo "==> Building"
make clean >/dev/null 2>&1 || true
make
[ -x ./halo-daemon ] || { echo "Build produced no halo-daemon binary." >&2; exit 1; }

# ---------------------------------------------------------------- device
if [ -z "$DEVICE" ]; then
    # Portable read loop rather than `mapfile` — that is a bash 4+ builtin,
    # and this way the script also runs under bash 3.2.
    DEV_LINES=()
    while IFS= read -r line; do DEV_LINES+=("$line"); done < <(enumerate_playback_devices)
    if [ "${#DEV_LINES[@]}" -eq 0 ]; then
        echo "No ALSA playback devices found under /proc/asound." >&2
        echo "Is the DAC plugged in, and is alsa-utils installed?" >&2
        aplay -l || true
        exit 1
    fi

    # Recommend the first USB device that advertises DSD, else the first USB
    # device, else the first device at all.
    RECOMMENDED=1
    for i in "${!DEV_LINES[@]}"; do
        case "${DEV_LINES[$i]}" in *", DSD)"*) RECOMMENDED=$((i + 1)); break ;; esac
    done
    if [ "$RECOMMENDED" -eq 1 ]; then
        for i in "${!DEV_LINES[@]}"; do
            case "${DEV_LINES[$i]}" in *"(USB"*) RECOMMENDED=$((i + 1)); break ;; esac
        done
    fi

    echo "==> Playback devices"
    for i in "${!DEV_LINES[@]}"; do
        n=$((i + 1))
        hw="${DEV_LINES[$i]%%$'\t'*}"
        label="${DEV_LINES[$i]#*$'\t'}"
        if [ "$n" -eq "$RECOMMENDED" ]; then
            printf '  %d) %-10s %s  <- recommended\n' "$n" "$hw" "$label"
        else
            printf '  %d) %-10s %s\n' "$n" "$hw" "$label"
        fi
    done
    echo

    CHOICE="$RECOMMENDED"
    if [ "$ASSUME_YES" -eq 0 ]; then
        read -rp "Choose [${RECOMMENDED}]: " reply
        [ -n "$reply" ] && CHOICE="$reply"
    fi
    [[ "$CHOICE" =~ ^[0-9]+$ ]] && [ "$CHOICE" -ge 1 ] && [ "$CHOICE" -le "${#DEV_LINES[@]}" ] \
        || { echo "Not a listed choice: $CHOICE" >&2; exit 1; }

    DEVICE="${DEV_LINES[$((CHOICE - 1))]%%$'\t'*}"
    echo "    Using $DEVICE"
fi

case "$DEVICE" in
    plughw:*|default*)
        echo
        echo "WARNING: '$DEVICE' goes through ALSA's conversion layer."
        echo "         Output will NOT be bit-perfect, and native DSD will not work."
        [ "$ASSUME_YES" -eq 1 ] || { read -rp "Continue anyway? [y/N] " a; [ "$a" = y ] || exit 1; }
        ;;
esac

echo
echo "==> Capabilities reported by $DEVICE"
# Informational: shows whether DSD_U32/U16/U8 appear (native DSD support)
# and the supported rate range, which is what decides the app's HALO DSD
# Mode setting. Never fatal — some drivers refuse this probe while busy.
aplay --dump-hw-params -D "$DEVICE" /dev/null 2>&1 | sed -n '/FORMAT:/,/^$/p' || true

# ------------------------------------------------------------------ user
if ! id -u halo >/dev/null 2>&1; then
    echo "==> Creating system user 'halo'"
    useradd --system --no-create-home --shell /usr/sbin/nologin halo
fi
# Idempotent; also repairs an existing install whose user lost audio access.
usermod -aG audio halo

# --------------------------------------------------------------- install
# Stop first: replacing a running executable gives ETXTBSY.
if systemctl is-active --quiet halo-daemon.service 2>/dev/null; then
    echo "==> Stopping running halo-daemon"
    systemctl stop halo-daemon.service
fi

echo "==> Installing binary to /usr/local/bin/halo-daemon"
install -m 0755 ./halo-daemon /usr/local/bin/halo-daemon

# journalctl keeps either the timestamps or the cover art's colour, never
# both. This is the reader that does — see README, "Timestamps and colour".
install -m 0755 ./tools/halo-log /usr/local/bin/halo-log

echo "==> Writing systemd unit (device=$DEVICE port=$PORT)"
sed -E "s#^ExecStart=.*#ExecStart=/usr/local/bin/halo-daemon ${DEVICE} ${PORT}#" \
    systemd/halo-daemon.service > /etc/systemd/system/halo-daemon.service
grep -q "^ExecStart=/usr/local/bin/halo-daemon ${DEVICE} ${PORT}$" \
    /etc/systemd/system/halo-daemon.service \
    || { echo "ExecStart substitution failed — check systemd/halo-daemon.service" >&2; exit 1; }

# Avahi is the entire discovery mechanism — halo-daemon does no mDNS itself,
# it just drops a static service file in /etc/avahi/services/. Raspberry Pi
# OS ships Avahi; minimal images (DietPi in particular) do not. Without it
# the endpoint is fully functional but simply never appears in the app, with
# no error on either side, which is a miserable thing to diagnose.
# Checked two ways on purpose. `systemctl list-unit-files` can come back
# without avahi for a moment right after apt has installed it — systemd is
# still reloading — and a false "not installed" here sends the reader off
# installing something they already have.
avahi_present() {
    command -v avahi-daemon >/dev/null 2>&1 && return 0
    systemctl list-unit-files 2>/dev/null | grep -q '^avahi-daemon\.service' && return 0
    return 1
}
if ! avahi_present; then
    AVAHI_MISSING=1
    echo
    echo "WARNING: avahi-daemon is not installed."
    echo "         The service file will still be written, but Audio Lounge"
    echo "         will NOT discover this endpoint until Avahi is running."
    echo "         DietPi:  dietpi-software install 152"
    echo "         Debian:  apt install avahi-daemon"
    echo
elif ! systemctl is-active --quiet avahi-daemon 2>/dev/null; then
    echo "==> Enabling avahi-daemon (needed for discovery)"
    systemctl enable --now avahi-daemon || true
fi

echo "==> Writing Avahi advertisement (port=$PORT)"
mkdir -p /etc/avahi/services
sed -E "s#<port>[0-9]+</port>#<port>${PORT}</port>#" \
    avahi/halo-daemon.service > /etc/avahi/services/halo-daemon.service
# Avahi picks up /etc/avahi/services changes on its own; no reload needed.

echo "==> Enabling and starting"
systemctl daemon-reload
systemctl enable halo-daemon.service >/dev/null
systemctl restart halo-daemon.service

sleep 1
echo
if systemctl is-active --quiet halo-daemon.service; then
    echo "==> halo-daemon is running on ${DEVICE}, port ${PORT}"
    echo
    systemctl --no-pager --lines=8 status halo-daemon.service || true
    echo
    echo "Follow the live status line with:"
    echo "    journalctl -u halo-daemon -f"
    echo
    if [ "${AVAHI_MISSING:-0}" -eq 1 ]; then
        echo "Discovery is NOT active (no avahi-daemon) — install it, then:"
        echo "    systemctl restart avahi-daemon"
    else
        echo "In Audio Lounge: Settings -> enable HALO, then pick"
        echo "    \"HALO Audio Transport on $(hostname)\""
    fi
else
    echo "==> halo-daemon FAILED to start. Recent log:" >&2
    journalctl -u halo-daemon --no-pager --lines=30 || true
    exit 1
fi
