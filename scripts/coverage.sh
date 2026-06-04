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

if command -v gcovr >/dev/null 2>&1; then
    mkdir -p build/coverage
    gcovr \
        --root . \
        --filter 'src/' \
        --exclude 'src/bsp/' \
        --exclude 'src/qpc/' \
        --exclude 'src/utils/template/' \
        --exclude 'src/utils/template_qpc_AO/' \
        --xml-pretty \
        --output build/coverage/coverage.xml \
        build/ceedling/test/out
    COVERAGE_XML_MSG='Coverage XML report: build/coverage/coverage.xml'
else
    COVERAGE_XML_MSG='Coverage XML report: skipped because gcovr was not found'
fi

printf '\nCoverage text report: build/ceedling/artifacts/gcov/gcovr/coverage.txt\n'
printf 'Coverage HTML report: build/ceedling/artifacts/gcov/gcovr/GcovCoverageResults.html\n'
printf '%s\n' "${COVERAGE_XML_MSG}"

printf '\nProduction service coverage:\n'
grep -E '^src/svc/video_pipeline/(video_io|video_modes|video_pipeline)\.c[[:space:]]' \
    build/ceedling/artifacts/gcov/gcovr/coverage.txt
