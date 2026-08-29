#!/usr/bin/env bash
set -euo pipefail

# Capture perf trace from the running board
# Usage: ./scripts/capture_perf_trace.sh [duration_seconds]

DURATION="${1:-30}"
BOARD_HOST="${BOARD_HOST:-root@192.168.1.10}"
LOCAL_OUTPUT="${LOCAL_OUTPUT:-logs/perf_trace.txt}"

step() {
    printf '\n==> %s\n' "$1"
}

step "Starting perf recording on board for ${DURATION} seconds"
cat <<'EOF_REMOTE' | ssh "${BOARD_HOST}" 'bash -s' "${DURATION}"
#!/usr/bin/env bash
DURATION="$1"

# Check if perf is available
if ! command -v perf >/dev/null 2>&1; then
    echo "ERROR: perf tool not found on board" >&2
    echo "Did you rebuild PetaLinux with perf enabled?" >&2
    exit 1
fi

# Check if subtitle_overlay_fw is running
PID=$(pidof subtitle_overlay_fw || echo "")
if [ -z "$PID" ]; then
    echo "WARNING: subtitle_overlay_fw not running, will capture system-wide" >&2
    PERF_TARGET="-a"
else
    echo "Found subtitle_overlay_fw at PID $PID"
    PERF_TARGET="-p $PID"
fi

echo "Recording for ${DURATION} seconds..."
# -g = call graphs (stack traces)
# -F 999 = sample frequency (per second)
# -a = all CPUs (or -p PID for specific process)
perf record $PERF_TARGET -g -F 999 -- sleep "$DURATION"

echo "Converting to text format..."
perf script -F time,comm,pid,tid,event,ip,sym > /tmp/perf_trace.txt

echo "Trace captured: /tmp/perf_trace.txt"
EOF_REMOTE

step "Copying trace from board"
mkdir -p "$(dirname "${LOCAL_OUTPUT}")"
scp "${BOARD_HOST}:/tmp/perf_trace.txt" "${LOCAL_OUTPUT}"

step "Cleaning up on board"
ssh "${BOARD_HOST}" 'rm -f /tmp/perf_trace.txt perf.data perf.data.old'

printf '\nCaptured perf trace: %s\n' "${LOCAL_OUTPUT}"
printf 'Convert to Perfetto with: python3 scripts/perf_to_perfetto.py %s\n' "${LOCAL_OUTPUT}"
