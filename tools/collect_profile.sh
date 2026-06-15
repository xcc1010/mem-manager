#!/usr/bin/env bash
# Collect mem-manager profile data from both planes and analyze it in one go.
#
#   CP plane -> the file backend writes a pure-JSONL file (shm_profile.<pid>.jsonl)
#   DP plane -> the log backend writes "[MODULE]{json}" lines into the OS log,
#               which is usually split across many rotated files in a directory.
#
# This script pulls the DP tagged lines out of every file under the given log
# directory, concatenates the CP JSONL, and feeds the combined stream to
# analyze_profile.py (which understands both the pure-JSONL and log-prefixed
# shapes and correlates map<->unmap per pid / [pg][vcpu] context).
#
# Usage:
#   tools/collect_profile.sh <cp.jsonl|-> <dp_log_dir> [module] [tsc_ghz]
#
#   <cp.jsonl>   path to the CP JSONL file, or "-" / a missing path to skip CP
#   <dp_log_dir> directory (searched recursively) holding the DP log chunks;
#                "-" to skip DP
#   [module]     log module tag, default SHMPROF
#   [tsc_ghz]    CPU TSC frequency in GHz, to convert DP TSC cycles to ns
#                (e.g. 2.5); omit to leave DP times in raw cycles
#
# Examples:
#   tools/collect_profile.sh shm_profile.1234.jsonl /var/log/dp SHMPROF 2.5
#   tools/collect_profile.sh - /var/log/dp                 # DP only
#   tools/collect_profile.sh shm_profile.1234.jsonl -      # CP only
set -euo pipefail

if [ "$#" -lt 2 ]; then
    grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'   # print the header as help
    exit 2
fi

CP_JSONL="$1"
DP_DIR="$2"
MODULE="${3:-SHMPROF}"
TSC_GHZ="${4:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
PY="$(command -v python3 || command -v python)"

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

cp_lines=0
dp_lines=0

# CP: pure JSONL, take as-is.
if [ "$CP_JSONL" != "-" ] && [ -f "$CP_JSONL" ]; then
    cat "$CP_JSONL" >> "$TMP"
    cp_lines="$(wc -l < "$CP_JSONL" | tr -d ' ')"
fi

# DP: pull our "[MODULE]" lines out of every file under the directory.
# -r recurse, -h drop filename prefix, -F fixed string (so the [] are literal).
if [ "$DP_DIR" != "-" ] && [ -e "$DP_DIR" ]; then
    if grep -rhF "[$MODULE]" "$DP_DIR" >> "$TMP" 2>/dev/null; then
        dp_lines="$(grep -rhF "[$MODULE]" "$DP_DIR" 2>/dev/null | wc -l | tr -d ' ')"
    fi
fi

echo "collected: CP=$cp_lines lines, DP=$dp_lines lines (module=$MODULE)" >&2
if [ ! -s "$TMP" ]; then
    echo "nothing collected — check the paths and the module tag." >&2
    exit 1
fi

set -- --module "$MODULE"
[ -n "$TSC_GHZ" ] && set -- "$@" --tsc-ghz "$TSC_GHZ"
exec "$PY" "$HERE/analyze_profile.py" "$@" "$TMP"
