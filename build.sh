#!/usr/bin/env bash
# Build maccorespeed tuned for the CPU of this Mac (or for another one with --cpu).
#
#   ./build.sh                 build for this machine, e.g. bin/maccorespeed-apple-m5
#   ./build.sh --cpu apple-m2  build for another Mac (e.g. to copy it to a machine without a compiler)
#
# The path of the binary is printed on stdout (last line), everything else goes to stderr.
set -euo pipefail
cd "$(dirname "$0")"

CPU=""
QUIET=0
while [ $# -gt 0 ]; do
    case "$1" in
        --cpu) CPU="${2:?--cpu needs a value}"; shift 2 ;;
        --cpu=*) CPU="${1#*=}"; shift ;;
        -q|--quiet) QUIET=1; shift ;;
        -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
log() { [ "$QUIET" = 1 ] || echo "$@" >&2; }

if ! xcode-select -p >/dev/null 2>&1; then
    echo "error: no compiler. Install the Xcode Command Line Tools with:  xcode-select --install" >&2
    exit 1
fi
# /usr/bin/clang is the system shim for the Apple clang of the active Xcode / Command Line Tools
# (it also selects the SDK), even if another clang such as Homebrew LLVM comes first in PATH.
CC=${CC:-/usr/bin/clang}

supports() { echo 'int main(void){return 0;}' | "$CC" "$@" -x c - -c -o /dev/null >/dev/null 2>&1; }

ARCH=$(uname -m)
BRAND=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)

# Pick the most specific -mcpu this compiler knows for this chip: "Apple M2 Pro" -> apple-m2,
# "Apple M5" -> apple-m5; a chip newer than the compiler falls back to the newest known generation.
if [ -z "$CPU" ]; then
    if [ "$ARCH" = arm64 ]; then
        FAMILY=$(printf '%s' "$BRAND" | sed -nE 's/^Apple ([AM])[0-9]+.*/\1/p' | tr 'AM' 'am')
        GEN=$(printf '%s' "$BRAND" | sed -nE 's/^Apple [AM]([0-9]+).*/\1/p')
        if [ -n "$GEN" ]; then
            for ((g = GEN + 0; g >= 1; g--)); do
                if supports "-mcpu=apple-$FAMILY$g"; then CPU="apple-$FAMILY$g"; break; fi
            done
        fi
        [ -z "$CPU" ] && supports -mcpu=native && CPU=native
        [ -z "$CPU" ] && CPU=generic
    else
        CPU=native
    fi
fi

if [ "$ARCH" = arm64 ]; then
    TUNE="-mcpu=$CPU"
    [ "$CPU" = generic ] && TUNE=""
else
    TUNE="-march=$CPU"
fi
# $TUNE is a single word or empty: left unquoted on purpose (bash 3.2 + set -u dislike empty arrays)
CFLAGS="-O3 $TUNE -std=gnu11 -Wall -Wextra -pthread -mmacosx-version-min=12.0"
if ! supports $CFLAGS; then
    echo "error: $CC does not accept: $CFLAGS" >&2
    exit 1
fi

CCVER=$("$CC" --version | head -1 | sed -E 's/ \(.*//')
OUT="bin/maccorespeed-$CPU"
mkdir -p bin
log "chip:     $BRAND ($ARCH)"
log "compiler: $CCVER"
log "flags:    $CFLAGS"
"$CC" $CFLAGS "-DBUILD_INFO=\"$CCVER, -O3 $TUNE\"" src/*.c \
    -framework IOKit -framework CoreFoundation -o "$OUT"
log "built:    $OUT"
echo "$OUT"
