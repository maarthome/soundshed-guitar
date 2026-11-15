#!/usr/bin/env bash
# run_pi_kiosk.sh — Show Soundshed Guitar full screen on the Pi's own display.
#
# For a Pi that boots to a text console (no desktop running): starts the labwc
# compositor on the local screen (tty7) and the app inside it with --kiosk.
# Works from an SSH session, because the display session is opened by systemd
# on the Pi's own seat rather than by the SSH login.
#
# Usage: ./run_pi_kiosk.sh [start|stop|restart|status|log] [app options]
#   start     Start the app on the local display (default)
#   stop      Close the app and the display session
#   restart   stop, then start
#   status    Show whether it is running
#   log       Follow the app's output (Ctrl+C to leave)
#
# Anything after the command is passed to the app after --kiosk.
#
# Environment:
#   SOUNDSHED_APP   Path to the executable
#                   (default: linux-dist-arm64/opt/Soundshed/soundshed-guitar/soundshed-guitar
#                    next to this script)
#   SOUNDSHED_VT    Virtual terminal to use (default: 7)
#   SOUNDSHED_AUDIO_PRIO
#                   Realtime priority of the app's audio thread, 1 to 95 (default: 70)
#
# Starting also sets the CPU governor to "performance". Left to save power, the Pi slows its
# clock when the load is low and is then late with an audio block now and then. It stays
# that way until the next reboot.
#
# Run it as your normal user, not with sudo: the script calls sudo itself.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT="soundshed-kiosk-test"
VT="${SOUNDSHED_VT:-7}"
AUDIO_PRIO="${SOUNDSHED_AUDIO_PRIO:-70}"
APP="${SOUNDSHED_APP:-${SCRIPT_DIR}/linux-dist-arm64/opt/Soundshed/soundshed-guitar/soundshed-guitar}"
# labwc settings for this session only: no title bar on the app, and it fills the screen.
LABWC_CONFIG_DIR="${SCRIPT_DIR}/tools/pi-kiosk/labwc"

if [[ "$(id -u)" -eq 0 ]]; then
    echo "Run this script as your normal user, not as root or with sudo." >&2
    exit 1
fi

is_running() {
    systemctl is-active --quiet "$UNIT"
}

start() {
    if is_running; then
        echo "Already running. Use '$0 restart' to start it again." >&2
        exit 1
    fi

    if [[ ! -x "$APP" ]]; then
        echo "✗ No executable at ${APP}" >&2
        echo "  Build first (./build_pi.sh), or set SOUNDSHED_APP." >&2
        exit 1
    fi

    if ! command -v labwc >/dev/null 2>&1; then
        echo "✗ labwc is not installed. Install it with: sudo apt install labwc xwayland" >&2
        exit 1
    fi

    # labwc runs its startup command through a shell, so quote every word.
    local app_cmd
    app_cmd="$(printf '%q ' "$APP" --kiosk "$@")"

    if [[ ! "$AUDIO_PRIO" =~ ^[0-9]+$ || "$AUDIO_PRIO" -lt 1 || "$AUDIO_PRIO" -gt 95 ]]; then
        echo "SOUNDSHED_AUDIO_PRIO must be a number from 1 to 95 (received: '${AUDIO_PRIO}')." >&2
        exit 1
    fi

    # Full clock speed, for audio without dropouts. Not every machine has the control.
    local governor
    for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        if [[ -e "$governor" ]]; then
            echo performance | sudo tee "$governor" >/dev/null || true
        fi
    done

    # A failed earlier run can leave the unit name taken.
    sudo systemctl reset-failed "$UNIT" 2>/dev/null || true

    sudo chvt "$VT"
    sudo systemd-run --unit="$UNIT" --collect --quiet \
        -p User="$(id -un)" \
        -p PAMName=login \
        -p TTYPath="/dev/tty${VT}" \
        -p StandardInput=tty-fail \
        -p StandardOutput=journal \
        -p StandardError=journal \
        -p LimitRTPRIO=95 \
        -p Environment="SOUNDSHED_AUDIO_PRIO=${AUDIO_PRIO}" \
        -p LimitMEMLOCK=infinity \
        "$(command -v labwc)" -C "$LABWC_CONFIG_DIR" -s "$app_cmd"

    echo "✓ Started on the local display (tty${VT})."
    echo "  Log:  $0 log"
    echo "  Stop: $0 stop"
}

stop() {
    if is_running; then
        sudo systemctl stop "$UNIT"
        echo "✓ Stopped."
    else
        echo "Not running."
    fi
    # Back to the text console.
    sudo chvt 1 2>/dev/null || true
}

command="${1:-start}"
[[ $# -gt 0 ]] && shift

case "$command" in
    start)   start "$@" ;;
    stop)    stop ;;
    restart) stop; start "$@" ;;
    status)
        if is_running; then
            echo "Running."
        else
            echo "Not running."
        fi
        ;;
    log)     exec journalctl -u "$UNIT" -f ;;
    -h|--help|help)
        sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        ;;
    *)
        echo "Unknown command: ${command}" >&2
        echo "Usage: $0 [start|stop|restart|status|log] [app options]" >&2
        exit 1
        ;;
esac
