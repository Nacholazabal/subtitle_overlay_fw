#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the SimulStreaming / AlignAtt end-to-end short-audio
# test. Same bench, reports and overlay reconstruction as audiotestshort.sh, but
# pinned to the SimulStreaming backend profile.
#
#   ./scripts/audiotestsimulstream.sh                 # one normal run + report
#   ./scripts/audiotestsimulstream.sh --sweep         # AlignAtt parameter sweep
#   ./scripts/audiotestsimulstream.sh --offline-only  # just build offline proxies
#
# Override the Colab endpoint (defaults to the shared static ngrok tunnel):
#   STT_STREAM_URL="wss://.../stt/stream" ./scripts/audiotestsimulstream.sh
#
# The bench aborts before playing any audio if /health does not report
# run_engine=simulstreaming_alignatt, so you never measure the wrong backend.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export STT_STREAM_URL="${STT_STREAM_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"
export AUDIO_TEST_PROFILE="simulstreaming_alignatt"

echo "SimulStreaming end-to-end test"
echo "  profile    : ${AUDIO_TEST_PROFILE}"
echo "  stream URL : ${STT_STREAM_URL}"

exec python3 "${REPO_DIR}/scripts/audio_test_short.py" --profile simulstreaming_alignatt "$@"
