#!/usr/bin/env bash
#
# ARCH-06 guard: every function exported from a project-owned header must have a
# production call site, or an entry in scripts/dead_symbols.ignore stating why it
# is deliberately kept.
#
# A symbol is "dead" when it is referenced fewer than 3 times across the
# non-vendored sources: prototype + definition + at least one call.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IGNORE_FILE="${SCRIPT_DIR}/dead_symbols.ignore"

cd "${REPO_ROOT}"

# Headers whose exports are audited. src/bsp/vtc_v7_2, src/bsp/qpc_port and
# src/qpc are vendored or project-owned copies of vendored code: their public
# surface is upstream's, not ours.
HEADER_GLOBS=(
    'src/app/*.h'
    'src/bsp/bsp_compat/*.h'
    'src/bsp/platform/*.h'
    'src/bsp/platform/linux/*.h'
    'src/hal/**/*.h'
    'src/svc/**/*.h'
    'src/utils/**/*.h'
)

# Where a call site may live. Same set plus the .c files next to them.
SOURCE_GLOBS=(
    'src/app/*.c'
    'src/bsp/bsp_compat/*.c'
    'src/bsp/platform/**/*.c'
    'src/bsp/qpc_port/*.c'
    'src/bsp/qpc_port/*.h'
    'src/hal/**/*.c'
    'src/svc/**/*.c'
    'src/utils/**/*.c'
)

mapfile -t HEADERS < <(git ls-files --cached --others --exclude-standard "${HEADER_GLOBS[@]}" | sort -u)
mapfile -t SOURCES < <(
    git ls-files --cached --others --exclude-standard "${SOURCE_GLOBS[@]}" "${HEADER_GLOBS[@]}" | sort -u
)

if [[ ${#HEADERS[@]} -eq 0 ]]; then
    printf 'dead_symbols: no headers selected\n' >&2
    exit 1
fi

# --- ignore list -------------------------------------------------------------
# Format: "symbol_name  # reason". A bare symbol with no reason is rejected, so
# the list can only be satisfied by explaining an entry, never by adding one.
declare -A IGNORED=()
if [[ -f "${IGNORE_FILE}" ]]; then
    lineno=0
    while IFS= read -r raw || [[ -n "${raw}" ]]; do
        lineno=$((lineno + 1))
        line="${raw%%$'\r'}"
        [[ -z "${line//[[:space:]]/}" ]] && continue
        [[ "${line}" =~ ^[[:space:]]*# ]] && continue

        symbol="${line%%#*}"
        symbol="${symbol//[[:space:]]/}"
        reason="${line#*#}"
        reason="$(printf '%s' "${reason}" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

        if [[ "${line}" != *'#'* || -z "${reason}" ]]; then
            printf '%s:%d: ignore entry "%s" has no reason after "#"\n' \
                "${IGNORE_FILE}" "${lineno}" "${symbol}" >&2
            exit 1
        fi
        IGNORED["${symbol}"]="${reason}"
    done < "${IGNORE_FILE}"
fi

# --- collect exported function names -----------------------------------------
# Strip comments and preprocessor lines, flatten to one stream, split on ';',
# and take the identifier in front of the first '(' of each declaration.
collect_symbols() {
    # A header with no function declarations makes grep exit 1; that is not an
    # error here, so this runs outside errexit.
    set +e
    local header
    for header in "${HEADERS[@]}"; do
        sed -e 's://.*::' "${header}" |
        sed -e '/^[[:space:]]*#/d' |
        tr '\n' ' ' |
        sed -e 's:/\*[^*]*\*\+\([^/*][^*]*\*\+\)*/: :g' |
        tr ';' '\n' |
        grep -E '\(' |
        grep -Ev '\b(typedef|extern[[:space:]]+"C"|return)\b' |
        sed -nE 's:^[^()]*[^A-Za-z0-9_]([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\(.*$:\1:p'
    done
    set -e
}

mapfile -t SYMBOLS < <(collect_symbols | sort -u | grep -Ev '^(if|for|while|switch|sizeof|defined)$' || true)

if [[ ${#SYMBOLS[@]} -eq 0 ]]; then
    printf 'dead_symbols: no exported symbols found; the header scan is broken\n' >&2
    exit 1
fi

# --- count references --------------------------------------------------------
failures=0
stale_ignores=()

for symbol in "${SYMBOLS[@]}"; do
    refs=$(grep -how "\<${symbol}\>" "${SOURCES[@]}" 2>/dev/null | wc -l)

    if [[ -n "${IGNORED[${symbol}]+set}" ]]; then
        if [[ "${refs}" -ge 3 ]]; then
            stale_ignores+=("${symbol}")
        fi
        continue
    fi

    if [[ "${refs}" -lt 3 ]]; then
        printf 'dead: %s (%s references in src/, expected prototype + definition + >=1 call)\n' \
            "${symbol}" "${refs}" >&2
        grep -Hn "\<${symbol}\>" "${SOURCES[@]}" 2>/dev/null | sed 's/^/      /' >&2
        failures=$((failures + 1))
    fi
done

if [[ ${#stale_ignores[@]} -gt 0 ]]; then
    printf '\n%s lists symbols that now have production callers; drop them:\n' "${IGNORE_FILE}" >&2
    printf '  %s\n' "${stale_ignores[@]}" >&2
    exit 1
fi

if [[ "${failures}" -gt 0 ]]; then
    printf '\n%d exported symbol(s) have no production caller.\n' "${failures}" >&2
    printf 'Wire one in, delete the symbol, or add it to %s with a reason.\n' "${IGNORE_FILE}" >&2
    exit 1
fi

printf 'dead_symbols: %d exported symbols checked, %d deliberately ignored, none dead\n' \
    "${#SYMBOLS[@]}" "${#IGNORED[@]}"
