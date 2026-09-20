#!/usr/bin/env bash
set -euo pipefail

# Collect the current board trace and combine it with the newest server trace
# downloaded by server/notebooks/nemotron_profiling.ipynb. All profiling and
# clock-alignment logic stays in the existing trace writers and merge_traces.py;
# this file only orchestrates discovery, copy and merge.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BOARD_MAC="${BOARD_MAC:-00:0a:35:00:1e:53}"
BOARD_IP="${BOARD_IP:-}"
BOARD_USER="${BOARD_USER:-root}"
BOARD_SSH_TARGET="${BOARD_SSH_TARGET:-}"
BOARD_SSH_IDENTITY="${BOARD_SSH_IDENTITY:-}"
BOARD_SSH_HOST_KEY_ALIAS="${BOARD_SSH_HOST_KEY_ALIAS:-subtitle-overlay-board}"
BOARD_SSH_OPTS=(
    -o "HostKeyAlgorithms=+ssh-rsa"
    -o "PubkeyAcceptedAlgorithms=+ssh-rsa"
    -o "HostKeyAlias=${BOARD_SSH_HOST_KEY_ALIAS}"
    -o "StrictHostKeyChecking=accept-new"
)

usage() {
    cat <<EOF
Usage: ${0##*/}

Collect the active firmware profiling trace, locate the newest server trace
downloaded by the Colab profiling notebook, and generate a Perfetto JSON file.

Normal use has no arguments:
  ./scripts/profile-report.sh
EOF
}

if [[ $# -gt 0 ]]; then
    if [[ $# -eq 1 && ( "$1" == "-h" || "$1" == "--help" ) ]]; then
        usage
        exit 0
    fi
    usage >&2
    exit 2
fi

if [[ -n "${BOARD_SSH_IDENTITY}" ]]; then
    BOARD_SSH_OPTS+=( -i "${BOARD_SSH_IDENTITY}" )
fi

find_latest_server_trace() {
    local -a roots
    local root candidate candidate_mtime
    local latest_path=""
    local latest_mtime=-1

    roots=("${REPO_ROOT}/logs/profiling/inbox")
    if [[ -d /mnt/c/Users ]]; then
        for root in /mnt/c/Users/*/Downloads; do
            [[ -d "${root}" ]] && roots+=("${root}")
        done
    fi

    shopt -s nullglob
    for root in "${roots[@]}"; do
        [[ -d "${root}" ]] || continue
        for candidate in "${root}"/server_trace-*.jsonl; do
            [[ -f "${candidate}" ]] || continue
            candidate_mtime="$(stat -c '%Y' "${candidate}")"
            if (( candidate_mtime > latest_mtime )); then
                latest_mtime="${candidate_mtime}"
                latest_path="${candidate}"
            fi
        done
    done
    shopt -u nullglob

    [[ -n "${latest_path}" ]] && printf '%s\n' "${latest_path}"
}

SERVER_TRACE="$(find_latest_server_trace || true)"
if [[ -z "${SERVER_TRACE}" ]]; then
    cat >&2 <<EOF
No encontré un server_trace descargado por Colab.

Detené una vez la última celda de nemotron_profiling.ipynb y aceptá la descarga.
El script busca automáticamente en Downloads de Windows y en:
  logs/profiling/inbox/
EOF
    exit 2
fi

require_trace_event() {
    local path="$1"
    local label="$2"
    local event="$3"

    if ! grep -Eq "\"name\"[[:space:]]*:[[:space:]]*\"${event}\"" "${path}"; then
        printf '%s trace is incomplete: missing %s in %s\n' \
            "${label}" "${event}" "${path}" >&2
        return 1
    fi
}

# Do this before touching the board. A missing trace_stats means the notebook
# was not stopped through its final cell and the buffered tail is not certified.
require_trace_event "${SERVER_TRACE}" "Server" "trace_start"
require_trace_event "${SERVER_TRACE}" "Server" "trace_stats"

if [[ -n "${BOARD_SSH_TARGET}" ]]; then
    BOARD_DISPLAY="${BOARD_SSH_TARGET} (explicit target)"
elif [[ -n "${BOARD_IP}" ]]; then
    BOARD_SSH_TARGET="${BOARD_USER}@${BOARD_IP}"
    BOARD_DISPLAY="${BOARD_IP} (explicit IP)"
else
    printf 'Locating board MAC %s ...\n' "${BOARD_MAC}"
    BOARD_IP="$(python3 "${SCRIPT_DIR}/board/find_board_ip.py" --mac "${BOARD_MAC}")"
    BOARD_SSH_TARGET="${BOARD_USER}@${BOARD_IP}"
    BOARD_DISPLAY="${BOARD_IP}"
fi

RUN_DIR="${REPO_ROOT}/logs/profiling/$(date -u +%Y%m%d-%H%M%S)"
FW_TRACE="${RUN_DIR}/fw_trace.jsonl"
LOCAL_SERVER_TRACE="${RUN_DIR}/server_trace.jsonl"
PERFETTO_TRACE="${RUN_DIR}/unified_trace.json"
SUMMARY="${RUN_DIR}/summary.txt"
mkdir -p "${RUN_DIR}"

printf 'Board       : %s\n' "${BOARD_DISPLAY}"
printf 'Server trace: %s\n' "${SERVER_TRACE}"
printf 'Output      : %s\n' "${RUN_DIR}"

BOARD_WAS_RUNNING=0
restore_board_service() {
    if [[ "${BOARD_WAS_RUNNING}" -eq 1 ]]; then
        printf 'Restarting subtitle-overlay service ...\n'
        ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" \
            "/etc/init.d/subtitle-overlay start" || true
        BOARD_WAS_RUNNING=0
    fi
}
trap restore_board_service EXIT
trap 'exit 130' INT TERM

if ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" \
    "/etc/init.d/subtitle-overlay status" >/dev/null 2>&1; then
    BOARD_WAS_RUNNING=1
    printf 'Stopping service briefly to flush the firmware trace ...\n'
    ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" \
        "/etc/init.d/subtitle-overlay stop"
fi

printf 'Copying firmware trace ...\n'
scp -O "${BOARD_SSH_OPTS[@]}" \
    "${BOARD_SSH_TARGET}:/tmp/fw_trace.jsonl" "${FW_TRACE}"
cp "${SERVER_TRACE}" "${LOCAL_SERVER_TRACE}"

restore_board_service
trap - EXIT INT TERM

require_trace_event "${FW_TRACE}" "Firmware" "trace_start"
require_trace_event "${FW_TRACE}" "Firmware" "trace_stats"

printf 'Generating unified Perfetto trace ...\n'
python3 "${SCRIPT_DIR}/merge_traces.py" \
    --fw "${FW_TRACE}" \
    --server "${LOCAL_SERVER_TRACE}" \
    --output "${PERFETTO_TRACE}" 2>&1 | tee "${SUMMARY}"

printf '\nProfiling report ready:\n'
printf '  Perfetto: %s\n' "${PERFETTO_TRACE}"
printf '  Summary : %s\n' "${SUMMARY}"
printf 'Open the Perfetto file at https://ui.perfetto.dev\n'
