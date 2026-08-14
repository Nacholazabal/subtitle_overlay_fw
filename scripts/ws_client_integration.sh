#!/usr/bin/env bash
set -euo pipefail

# Integration check for the firmware WebSocket client.
#
# Builds the production client on the host and runs it against a fake server
# that imports server/runtime/protocol.py, so session_start is validated by the
# same code the Colab server uses and audio frames are decoded with the same
# CHUNK_HEADER. Everything runs locally: no board, no VM, no Colab.
#
# What this does NOT cover: TLS against a real certificate (the endpoint here is
# ws://) and FastAPI routing (fastapi/uvicorn are not installed in WSL). Those
# are covered by the negative TLS checks and by the real Colab run.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR="build/ws-integration"
PROBE="${BUILD_DIR}/ws_client_probe"
PORT="${WS_INTEGRATION_PORT:-8799}"
CHUNKS="${WS_INTEGRATION_CHUNKS:-25}"
SERVER_LOG="${BUILD_DIR}/server.log"
PROBE_LOG="${BUILD_DIR}/probe.log"
SERVER_SUMMARY="${BUILD_DIR}/server-summary.json"

mkdir -p "${BUILD_DIR}"
rm -f "${SERVER_SUMMARY}" "${SERVER_LOG}" "${PROBE_LOG}"

step() {
    printf '\n==> %s\n' "$1"
}

step "Building the host probe"
gcc -std=gnu99 -Wall -Wextra -O1 -DCONFIG_LOG_ENABLED \
    -Isrc/app -Isrc/hal/net_tls -Isrc/svc/stt -Isrc/utils/log -Isrc/utils/number_parse \
    -Isrc/qpc/include -Isrc/bsp/qpc_port -Isrc/qpc/ports/config \
    -o "${PROBE}" \
    test/integration/ws_client/ws_client_probe.c \
    src/svc/stt/stt_ws_client.c \
    src/svc/stt/stt_ws_frame.c \
    src/svc/stt/stt_session_json.c \
    src/svc/stt/stt_json.c \
    src/svc/stt/stt_transcript_parse.c \
    src/utils/log/log.c \
    src/utils/number_parse/number_parse.c \
    src/hal/net_tls/net_tls.c \
    -pthread -lssl -lcrypto

step "Starting the fake STT server on port ${PORT}"
python3 test/integration/ws_client/fake_stt_server.py \
    --port "${PORT}" \
    --expect-chunks "${CHUNKS}" \
    --summary "${SERVER_SUMMARY}" \
    >"${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for _ in $(seq 1 50); do
    grep -q "listening on" "${SERVER_LOG}" 2>/dev/null && break
    sleep 0.1
done
if ! grep -q "listening on" "${SERVER_LOG}" 2>/dev/null; then
    echo "the fake server never came up:" >&2
    cat "${SERVER_LOG}" >&2
    exit 1
fi

step "Running the probe against ws://127.0.0.1:${PORT}/stt/stream"
"${PROBE}" "ws://127.0.0.1:${PORT}/stt/stream" "${CHUNKS}" | tee "${PROBE_LOG}"

wait "${SERVER_PID}" || true
trap - EXIT

step "Checking the result"
python3 - "${SERVER_SUMMARY}" "${PROBE_LOG}" "${CHUNKS}" <<'PY'
import json
import sys

summary_path, probe_log, expected_chunks = sys.argv[1], sys.argv[2], int(sys.argv[3])
summary = json.loads(open(summary_path, encoding="utf-8").read())
probe = json.loads([l for l in open(probe_log, encoding="utf-8") if l.startswith("{")][-1])

failures = []

if summary["errors"]:
    failures.append(f"server reported errors: {summary['errors']}")

start = summary["session_start"] or {}
for key, want in (("version", 1), ("channels", 1), ("format", 1),
                  ("sample_rate_hz", 48000), ("chunk_ms", 20),
                  ("samples_per_chunk", 960), ("bytes_per_chunk", 1920)):
    if start.get(key) != want:
        failures.append(f"session_start.{key} = {start.get(key)!r}, expected {want!r}")

backend = summary["backend_config"] or {}
for key, want in (("target_lang", "es-ES"), ("latency_ms", 560),
                  ("stop_history_eou_ms", 600), ("residue_tokens_at_end", 2)):
    if backend.get(key) != want:
        failures.append(f"backend_config.{key} = {backend.get(key)!r}, expected {want!r}")

if summary["audio_frames"] < expected_chunks:
    failures.append(f"server saw {summary['audio_frames']} chunks, expected >= {expected_chunks}")
if summary["first_seq"] != 0:
    failures.append(f"first audio seq was {summary['first_seq']}, expected 0")
if summary["seq_gaps"]:
    failures.append(f"{summary['seq_gaps']} gaps in the audio sequence")
if summary["audio_bytes"] != summary["audio_frames"] * 1920:
    failures.append("audio payload sizes do not match 1920 bytes per chunk")

if probe["sessions"] != 1:
    failures.append(f"client opened {probe['sessions']} sessions, expected 1")
if probe["reconnects"]:
    failures.append(f"client reconnected {probe['reconnects']} times")
if probe["protocol_errors"]:
    failures.append(f"client reported {probe['protocol_errors']} protocol errors")
if probe["chunks_dropped"]:
    failures.append(f"client dropped {probe['chunks_dropped']} chunks")
if probe["transcripts_delivered"] < 1:
    failures.append("client delivered no transcripts")
if probe["state"] != "ready":
    failures.append(f"client ended in state {probe['state']!r}, expected 'ready'")

if failures:
    print("\nINTEGRATION FAILED:")
    for f in failures:
        print(f"  - {f}")
    raise SystemExit(1)

print(f"\nOK: {summary['audio_frames']} chunks accepted, "
      f"{probe['transcripts_delivered']} transcripts delivered, one session, no drops.")
PY

# --- TLS validation checks (need outbound HTTPS; skipped when offline) --------
#
# The whole point of wss:// is that a bad certificate stops the session. These
# assert that it actually does, and that a good one still gets through, so a
# broken verify path cannot pass unnoticed as "everything connects".

step "Checking TLS certificate validation"
if ! timeout 10 getent hosts expired.badssl.com >/dev/null 2>&1; then
    echo "SKIPPED: no DNS/outbound access from this host"
    exit 0
fi

tls_failures=0

# Collect the output first: piping straight into `grep -q` makes grep exit on
# the first match, and the resulting SIGPIPE would fail the probe under
# `set -o pipefail`, turning a correct rejection into a reported failure.
probe_output() {
    timeout 30 "${PROBE}" "$1" 1 2>&1 || true
}

for host in expired.badssl.com wrong.host.badssl.com self-signed.badssl.com; do
    if probe_output "wss://${host}/stt/stream" | grep -q "certificate verify failed"; then
        echo "  rejected ${host} (as required)"
    else
        echo "  FAILED: ${host} was not rejected" >&2
        tls_failures=$((tls_failures + 1))
    fi
done

# A control with a valid chain: the upgrade may still fail (that endpoint does
# not speak our protocol), but the handshake must not fail on the certificate.
# Match the failure phrasing, not the word "certificate": the configuration
# log line contains the CA bundle path (ca-certificates.crt) on every run.
if probe_output "wss://echo.websocket.org/" | grep -qE "certificate verify failed|certificate rejected"; then
    echo "  FAILED: a valid certificate was rejected" >&2
    tls_failures=$((tls_failures + 1))
else
    echo "  accepted echo.websocket.org (valid chain)"
fi

if [[ "${tls_failures}" -ne 0 ]]; then
    echo "TLS validation checks failed" >&2
    exit 1
fi
echo "TLS validation behaves correctly."
