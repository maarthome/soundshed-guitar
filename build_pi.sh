#!/usr/bin/env bash
# build_pi.sh — Run build_linux.sh with a temporary swap file.
#
# Creates a swap file, enables it, runs the normal Linux build, and then
# disables and deletes the swap file again — also when the build fails or is
# interrupted with Ctrl+C.
#
# Usage: ./build_pi.sh [build_linux.sh options]
#   Every argument is passed straight through to build_linux.sh, for example:
#     ./build_pi.sh --jobs 2
#     ./build_pi.sh --skip-ts
#
# Environment:
#   SWAP_SIZE_GB   Size of the temporary swap file in GB (default: 4)
#   SWAP_FILE      Where to put it (default: /var/tmp/soundshed-build.swap)
#
# Run it as your normal user, not with sudo: the script calls sudo itself for
# the swap commands only, so the build output stays owned by you.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SWAP_SIZE_GB="${SWAP_SIZE_GB:-4}"
SWAP_FILE="${SWAP_FILE:-/var/tmp/soundshed-build.swap}"

if [[ "$(id -u)" -eq 0 ]]; then
    echo "Run this script as your normal user, not as root or with sudo." >&2
    exit 1
fi

if [[ ! "$SWAP_SIZE_GB" =~ ^[0-9]+$ || "$SWAP_SIZE_GB" -lt 1 ]]; then
    echo "SWAP_SIZE_GB must be a positive whole number (received: '${SWAP_SIZE_GB}')." >&2
    exit 1
fi

SWAP_CREATED=false
SUDO_KEEPALIVE_PID=""

swap_is_active() {
    swapon --noheadings --show=NAME 2>/dev/null | grep -Fxq "$SWAP_FILE"
}

cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM

    if [[ -n "$SUDO_KEEPALIVE_PID" ]]; then
        kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true
    fi

    if [[ "$SWAP_CREATED" == true ]]; then
        echo ""
        echo "▶ Removing temporary swap file…"
        if swap_is_active; then
            sudo swapoff "$SWAP_FILE" || echo "  ⚠ swapoff failed for ${SWAP_FILE}" >&2
        fi
        if sudo rm -f "$SWAP_FILE"; then
            echo "  ✓ Swap file removed"
        else
            echo "  ⚠ Could not delete ${SWAP_FILE}; remove it by hand." >&2
        fi
    fi

    exit "$exit_code"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "▶ Preparing ${SWAP_SIZE_GB} GB temporary swap at ${SWAP_FILE}…"

# Ask for the sudo password once, up front, and keep the ticket alive so the
# cleanup at the end of a long build does not stop to ask again.
sudo -v
( while true; do sleep 60; sudo -n -v 2>/dev/null || exit; done ) &
SUDO_KEEPALIVE_PID=$!

# A swap file left behind by an earlier run that was killed hard.
if swap_is_active; then
    echo "  • Found an active swap file from an earlier run; turning it off first."
    sudo swapoff "$SWAP_FILE"
fi
if sudo test -e "$SWAP_FILE"; then
    sudo rm -f "$SWAP_FILE"
fi

SWAP_DIR="$(dirname "$SWAP_FILE")"
needed_kb=$(( SWAP_SIZE_GB * 1024 * 1024 + 512 * 1024 ))
available_kb="$(df --output=avail -k "$SWAP_DIR" | tail -n 1 | tr -d ' ')"
if [[ "$available_kb" -lt "$needed_kb" ]]; then
    echo "✗ Not enough free disk space in ${SWAP_DIR} for a ${SWAP_SIZE_GB} GB swap file." >&2
    echo "  Free: $(( available_kb / 1024 )) MB, needed: $(( needed_kb / 1024 )) MB." >&2
    echo "  Free up space, lower SWAP_SIZE_GB, or point SWAP_FILE at another disk." >&2
    exit 1
fi

SWAP_CREATED=true

make_swap_with() {
    # $1: "fallocate" or "dd"
    sudo rm -f "$SWAP_FILE"
    if [[ "$1" == "fallocate" ]]; then
        sudo fallocate -l "${SWAP_SIZE_GB}G" "$SWAP_FILE" || return 1
    else
        sudo dd if=/dev/zero of="$SWAP_FILE" bs=1M count=$(( SWAP_SIZE_GB * 1024 )) status=progress || return 1
    fi
    sudo chmod 600 "$SWAP_FILE"
    sudo mkswap "$SWAP_FILE" >/dev/null || return 1
    sudo swapon "$SWAP_FILE" || return 1
}

# fallocate is instant; on filesystems where the result cannot be used as swap,
# fall back to writing the file out with dd.
if ! { command -v fallocate >/dev/null 2>&1 && make_swap_with fallocate 2>/dev/null; }; then
    echo "  • fallocate not usable here; writing the swap file with dd (slower)…"
    make_swap_with dd
fi

echo "  ✓ Swap enabled"
swapon --show
echo ""

build_status=0
bash "${SCRIPT_DIR}/build_linux.sh" "$@" || build_status=$?

if [[ "$build_status" -eq 0 ]]; then
    echo ""
    echo "✓ Build finished."
else
    echo ""
    echo "✗ Build failed with exit code ${build_status}." >&2
fi

exit "$build_status"
