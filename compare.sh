#!/usr/bin/env bash
# Side-by-side comparison of maccorespeed logs, e.g. the runs of two different Macs:
#
#   ./compare.sh results/Apple-M5-*.txt results/Apple-M2-Pro-*.txt
set -euo pipefail
if [ $# -lt 1 ]; then
    echo "usage: $0 LOG [LOG ...]" >&2
    exit 2
fi

awk '
function num(line,   m) { return match(line, /score +[0-9]+/) ? substr(line, RSTART + 5) + 0 : "" }
FNR == 1 { n++; chip[n] = FILENAME }
/^SUMMARY / {
    # "SUMMARY  MacBook Air (15-inch, M5), Apple M5, 10 cores (...)"
    line = substr($0, 10)
    if (match(line, /, Apple [^,]+,/)) {
        model[n] = substr(line, 1, RSTART - 1)
        chip[n] = substr(line, RSTART + 2, RLENGTH - 3)
    }
}
/^  Single-core score/          { st[n] = num($0) }
/^  Multi-core burst score/     { bu[n] = num($0) }
/^  Multi-core sustained score/ { su[n] = num($0) }
/^  Steady state reached after/ { ss[n] = $5 }
/^  Burst clock and power/      { bp[n] = $0; sub(/.*CPU /, "", bp[n]) }
/^  Sustained clock and power/  { sp[n] = $0; sub(/.*CPU /, "", sp[n]) }
function row(label, v,   i) {
    printf "%-24s", label
    for (i = 1; i <= n; i++) printf "%18s", (i in v) ? v[i] : "-"
    if (n == 2 && (1 in v) && (2 in v) && v[2] + 0 > 0 && v[1] ~ /^[0-9.]+$/)
        printf "%12.2f", v[1] / v[2]
    printf "\n"
}
END {
    for (i = 1; i <= n; i++) if ((i in bu) && (i in su) && bu[i] > 0) ratio[i] = sprintf("%.1f%%", 100 * su[i] / bu[i])
    for (i = 1; i <= n; i++) if ((i in st) && (i in bu) && st[i] > 0) scal[i] = sprintf("%.2fx", bu[i] / st[i])
    printf "%-24s", ""
    for (i = 1; i <= n; i++) printf "%18s", chip[i]
    if (n == 2) printf "%12s", "1st / 2nd"
    printf "\n"
    for (i = 1; i <= n; i++) printf "%s%s", (i == 1 ? "" : "  |  "), model[i]
    printf "\n\n"
    row("Single-core", st)
    row("Multi-core burst", bu)
    row("Multi-core sustained", su)
    row("Burst / single-core", scal)
    row("Sustained / burst", ratio)
    row("Steady state after", ss)
    row("CPU power burst", bp)
    row("CPU power sustained", sp)
}' "$@"
