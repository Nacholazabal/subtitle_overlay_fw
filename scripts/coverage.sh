#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

if command -v bundle >/dev/null 2>&1; then
    CEEDLING=(bundle exec ceedling)
else
    CEEDLING=(ceedling)
fi

"${CEEDLING[@]}" clean
"${CEEDLING[@]}" test:all
"${CEEDLING[@]}" gcov:all

printf '\nCoverage text report: build/ceedling/artifacts/gcov/gcovr/coverage.txt\n'
printf 'Coverage HTML report: build/ceedling/artifacts/gcov/gcovr/GcovCoverageResults.html\n'

printf '\nProduction service coverage:\n'
grep -E '^src/svc/video_pipeline/(video_modes|video_pipeline)\.c[[:space:]]' \
    build/ceedling/artifacts/gcov/gcovr/coverage.txt
