#!/usr/bin/env bash
# uninstall.sh — remove halo-daemon and its service definitions.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }

echo "==> Stopping and disabling service"
systemctl stop halo-daemon.service 2>/dev/null || true
systemctl disable halo-daemon.service 2>/dev/null || true

rm -f /etc/systemd/system/halo-daemon.service
rm -f /etc/avahi/services/halo-daemon.service
rm -f /usr/local/bin/halo-daemon /usr/local/bin/halo-log
systemctl daemon-reload

echo "==> Removed binary, systemd unit and Avahi advertisement."
# The user account is left alone on purpose: deleting it would orphan any
# file it owns, and it costs nothing to keep. Remove it yourself with
# `sudo userdel halo` if you really want it gone.
echo "    The 'halo' user was left in place (sudo userdel halo to remove)."
