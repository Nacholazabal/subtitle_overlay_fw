#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the Nemotron 3.5 / NeMo end-to-end short-audio test.
# Same bench, reports and overlay reconstruction as audiotestshort.sh, but pinned
# to the Nemotron backend profile.
#
#   ./scripts/audiotestnemotron.sh                 # one normal run + report (320 ms smoke)
#   ./scripts/audiotestnemotron.sh --offline-only  # just build offline proxies
#   ./scripts/audiotestnemotron.sh --sweep         # ten-run follow-up parameter sweep
#
# Override the Colab endpoint (defaults to the shared static ngrok tunnel):
#   STT_STREAM_URL="wss://.../stt/stream" ./scripts/audiotestnemotron.sh
#
# The bench aborts before playing any audio if /health does not report
# run_engine=nemotron_3_5_nemo, so you never measure the wrong backend.
#
# The default sweep repeats 320/560 ms, probes 1120 ms and changes one live
# endpointing parameter at a time around 560 ms. It does not reload the Colab model.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export STT_STREAM_URL="${STT_STREAM_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"
export AUDIO_TEST_PROFILE="nemotron_3_5_nemo"

echo "Nemotron end-to-end test"
echo "  profile    : ${AUDIO_TEST_PROFILE}"
echo "  stream URL : ${STT_STREAM_URL}"

exec python3 "${REPO_DIR}/scripts/audio_test_short.py" --profile nemotron_3_5_nemo "$@"
