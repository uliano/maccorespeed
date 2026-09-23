#!/usr/bin/env bash
# Build maccorespeed for this Mac (if a compiler is available) and run it, saving the output and
# the sustained time series in results/.
#
#   ./run.sh              full run: single-core, multi-core burst, multi-core sustained
#   ./run.sh --quick      short run, only to check that everything works
#   ./run.sh --help       all the options
set -euo pipefail
cd "$(dirname "$0")"

if xcode-select -p >/dev/null 2>&1; then
    BIN=$(./build.sh --quiet)
else
    # No compiler: use a binary built on another Mac with ./build.sh --cpu apple-mN
    GEN=$(sysctl -n machdep.cpu.brand_string | sed -nE 's/^Apple M([0-9]+).*/\1/p')
    BIN=""
    for ((g = ${GEN:-0}; g >= 1; g--)); do
        if [ -x "bin/maccorespeed-apple-m$g" ]; then BIN="bin/maccorespeed-apple-m$g"; break; fi
    done
    if [ -z "$BIN" ]; then
        echo "error: no compiler and no prebuilt binary for this Mac." >&2
        echo "Install the Xcode Command Line Tools (xcode-select --install), or run" >&2
        echo "./build.sh --cpu apple-m${GEN:-N} on another Mac and copy the bin/ folder here." >&2
        exit 1
    fi
    echo "no compiler found, using the prebuilt $BIN" >&2
fi

case " $* " in
    *" -h "* | *" --help "* | *" --list-sensors "* | *" --selftest "*) exec "$BIN" "$@" ;;
esac

mkdir -p results
CHIP=$(sysctl -n machdep.cpu.brand_string | tr -cs 'A-Za-z0-9' '-' | sed -E 's/^-|-$//g')
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="results/$CHIP-$STAMP.txt"
CSV="results/$CHIP-$STAMP.csv"

# tee -i ignores Ctrl-C, so the summary printed after an interrupt still reaches the log
"$BIN" --csv "$CSV" "$@" 2>&1 | tee -i "$LOG"
echo "saved: $LOG"
