#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Benchmarks one mounted volume (plan Step 11).
#
#   Bench/run.sh <mountpoint> <label> [--runs N] [--mib M] [--small N] [--meta N] [--device rdiskN]
#                [--remount CMD]
#
# Tests (median of N runs, default 5):
#   seq_write        M MiB (default 1024) in 4 MiB writes, then fsync
#   seq_read_cold    the same file read with F_NOCACHE after CMD remounted the volume (so no
#                    cached page survives the write; without --remount it is F_NOCACHE only)
#   seq_read_warm    read again through the page cache
#   small_files      N files of 1-16 KiB in 100 directories (default 30000), then rm -rf
#   metadata         create, rename and delete N empty files (default 10000)
#   raw_read         with --device: dd of /dev/rdiskN, 4 MiB blocks (the device's own speed)
# Writes Bench/results/<date>-<commit>-<label>.json with the machine, macOS build and setup.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 2 ] || { sed -n 4,14p "$0" >&2; exit 64; }
mnt="$1" label="$2"; shift 2
runs=5 mib=1024 small=30000 meta=10000 device="" remount=""
while [ $# -gt 0 ]; do
    case "$1" in
        --runs) runs="$2"; shift 2 ;;
        --mib) mib="$2"; shift 2 ;;
        --small) small="$2"; shift 2 ;;
        --meta) meta="$2"; shift 2 ;;
        --device) device="$2"; shift 2 ;;
        --remount) remount="$2"; shift 2 ;;
        *) echo "run.sh: unknown option $1" >&2; exit 64 ;;
    esac
done

io="$root/build/bench/io"
mkdir -p "$root/build/bench" "$root/Bench/results"
[ "$io" -nt "$root/Bench/io.c" ] || clang -O2 -Wall -Wextra -Werror -o "$io" "$root/Bench/io.c"

work="$mnt/unlatch-bench"
rm -rf "$work"; mkdir -p "$work"

median() { sort -n | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}'; }
mbps() { awk -v s="$1" -v m="$mib" 'BEGIN {printf "%.1f", m * 1048576 / s / 1e6}'; }

# macOS bash 3.2 has no associative arrays: the median for test T is in $s_T.
sec() { local v="s_$1"; echo "${!v:-}"; }
measure() {             # name, command...; median seconds over the runs
    local name="$1"; shift
    local all=()
    for i in $(seq 1 "$runs"); do all+=("$("$@")"); done
    printf -v "s_$name" '%s' "$(printf '%s\n' "${all[@]}" | median)"
    printf '  %-14s %s s (median of %d)\n' "$name" "$(sec "$name")" "$runs" >&2
}

echo "bench: $label on $mnt" >&2
measure seq_write "$io" write "$work/seq.bin" "$mib"
cold_run() {
    [ -z "$remount" ] || sh -c "$remount" >&2
    "$io" read "$work/seq.bin" nocache
}
measure seq_read_cold cold_run
"$io" read "$work/seq.bin" > /dev/null
measure seq_read_warm "$io" read "$work/seq.bin"
small_run() {
    local d="$work/small"; rm -rf "$d"; mkdir -p "$d"
    "$io" small "$d" "$small" 7
    rm -rf "$d"
}
measure small_files small_run
meta_run() { local d="$work/meta"; rm -rf "$d"; mkdir -p "$d"; "$io" meta "$d" "$meta"; rmdir "$d"; }
measure metadata meta_run
if [ -n "$device" ]; then
    raw_run() {
        local t0 t1
        t0=$(perl -MTime::HiRes=time -e 'printf "%.6f", time')
        dd if="/dev/$device" of=/dev/null bs=4m count=$((mib / 4)) 2>/dev/null
        t1=$(perl -MTime::HiRes=time -e 'printf "%.6f", time')
        awk -v a="$t0" -v b="$t1" 'BEGIN {printf "%.6f\n", b - a}'
    }
    measure raw_read raw_run
fi
rm -rf "$work"

commit=$(git -C "$root" rev-parse --short HEAD)
out="$root/Bench/results/$(date +%Y-%m-%d)-$commit-$label.json"
{
    echo "{"
    echo "  \"label\": \"$label\","
    echo "  \"commit\": \"$commit\","
    echo "  \"date\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
    echo "  \"macos\": \"$(sw_vers -productVersion) ($(sw_vers -buildVersion))\","
    echo "  \"machine\": \"$(sysctl -n machdep.cpu.brand_string), $(( $(sysctl -n hw.memsize) >> 30 )) GiB\","
    echo "  \"mount\": \"$(mount | grep " on $(cd "$mnt" && pwd) " | sed 's/"/\\"/g' | head -1)\","
    echo "  \"runs\": $runs, \"mib\": $mib, \"small_files\": $small, \"metadata_files\": $meta,"
    echo "  \"cold_reads\": \"$([ -n "$remount" ] && echo "remount + F_NOCACHE" || echo "F_NOCACHE only")\","
    echo "  \"seconds\": {"
    first=1
    for k in seq_write seq_read_cold seq_read_warm small_files metadata raw_read; do
        [ -n "$(sec $k)" ] || continue
        [ $first = 1 ] || echo ","
        printf '    "%s": %s' "$k" "$(sec $k)"
        first=0
    done
    echo ""
    echo "  },"
    echo "  \"mb_per_s\": {"
    printf '    "seq_write": %s, "seq_read_cold": %s, "seq_read_warm": %s' \
        "$(mbps "$(sec seq_write)")" "$(mbps "$(sec seq_read_cold)")" "$(mbps "$(sec seq_read_warm)")"
    [ -n "$(sec raw_read)" ] && printf ', "raw_read": %s' "$(mbps "$(sec raw_read)")"
    echo ""
    echo "  },"
    printf '  "small_files_per_s": %s,\n' "$(awk -v s="$(sec small_files)" -v n="$small" 'BEGIN {printf "%.0f", n / s}')"
    printf '  "metadata_ops_per_s": %s\n' "$(awk -v s="$(sec metadata)" -v n="$meta" 'BEGIN {printf "%.0f", 3 * n / s}')"
    echo "}"
} > "$out"
echo "bench: wrote $out" >&2
