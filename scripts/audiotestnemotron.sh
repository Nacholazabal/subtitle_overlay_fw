#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the Nemotron 3.5 / NeMo end-to-end short-audio test.
# Same bench, reports and overlay reconstruction as audiotestshort.sh, but pinned
# to the Nemotron backend profile.
#
#   ./scripts/audiotestnemotron.sh                 # one normal run + report (320 ms smoke)
#   ./scripts/audiotestnemotron.sh --offline-only  # just build offline proxies
#
# Override the Colab endpoint (defaults to the shared static ngrok tunnel):
#   STT_STREAM_URL="wss://.../stt/stream" ./scripts/audiotestnemotron.sh
#
# The bench aborts before playing any audio if /health does not report
# run_engine=nemotron_3_5_nemo, so you never measure the wrong backend.
#
# --sweep is deliberately NOT available yet: the 320 ms smoke test must pass end
# to end (stable /health, EOU finals, 100% firmware ACKs, no growing backlog)
# before an 80/320/560 sweep is worth running.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export STT_STREAM_URL="${STT_STREAM_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"
export AUDIO_TEST_PROFILE="nemotron_3_5_nemo"

echo "Nemotron end-to-end test"
echo "  profile    : ${AUDIO_TEST_PROFILE}"
echo "  stream URL : ${STT_STREAM_URL}"

exec python3 "${REPO_DIR}/scripts/audio_test_short.py" --profile nemotron_3_5_nemo "$@"
