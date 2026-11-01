#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the fsx randomized model test over many seeds in parallel, each on a fresh clone of
# one empty 512 MB image.
#
#   Scripts/fsx-soak.sh [--seeds N] [--start S] [--ops M] [--jobs J]   (default 100 seeds from 1, 2000 ops)
#   Scripts/fsx-soak.sh --regressions                                   seeds in Tests/fsx-regressions.txt
#
# Failing seeds are appended to Tests/fsx-regressions.txt; their images and logs stay in
# build/fsx/fail-<seed>.{img,log}.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
seeds=100 start=1 ops=2000 jobs="$(sysctl -n hw.ncpu)" regressions=0
while [ $# -gt 0 ]; do
    case "$1" in
        --seeds) seeds="$2"; shift 2 ;;
        --start) start="$2"; shift 2 ;;
        --ops) ops="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        --regressions) regressions=1; shift ;;
        *) echo "usage: $0 [--seeds N] [--start S] [--ops M] [--jobs J] | --regressions" >&2; exit 64 ;;
    esac
done

eng="$root/build/DerivedData/Build/Products/Debug/unlatch-engine"
out="$root/build/fsx"
[ -f build/ntfs3g/lib/libntfs-3g.a ] || Scripts/build-ntfs3g.sh
[ -d Unlatch.xcodeproj ] || xcodegen generate >/dev/null
mkdir -p "$out"
xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build > "$out/xcodebuild.log" 2>&1 \
    || { grep -E "error:" "$out/xcodebuild.log" >&2; echo "fsx-soak: build failed" >&2; exit 1; }

# Per-run template, so concurrent soaks (test-engine.sh, a manual soak) don't share one.
base="base-$$"
rm -f "$out/$base.img"
(cd "$out" && "$root/Scripts/make-image.sh" "$base" 512m FSX > /dev/null)
trap 'rm -f "$out/$base.img"' EXIT

if [ "$regressions" = 1 ]; then
    list="$(grep -v '^#' Tests/fsx-regressions.txt | awk 'NF >= 2 { print $1 " " $2 }')"
else
    list="$(seq "$start" $((start + seeds - 1)) | awk -v ops="$ops" '{ print $1 " " ops }')"
fi
count="$(printf '%s\n' "$list" | grep -c . || true)"
echo "fsx-soak: $count seeds, $jobs jobs"

export eng out base
run_one() {
    local seed="$1" ops="$2" img="$out/run-$1.img"
    cp -c "$out/$base.img" "$img"
    if "$eng" fsx "$img" --seed "$seed" --ops "$ops" > "$out/run-$seed.log" 2>&1; then
        rm -f "$img" "$out/run-$seed.log"
        echo "ok $seed"
    else
        mv "$img" "$out/fail-$seed.img"
        mv "$out/run-$seed.log" "$out/fail-$seed.log"
        echo "FAIL $seed $ops"
    fi
}
export -f run_one

start_s=$SECONDS
results="$(printf '%s\n' "$list" | xargs -P "$jobs" -L 1 bash -c 'run_one "$0" "$1"')"
failed="$(printf '%s\n' "$results" | grep '^FAIL' || true)"
passed="$(printf '%s\n' "$results" | grep -c '^ok' || true)"
echo "fsx-soak: $passed of $count seeds passed in $((SECONDS - start_s)) s"
if [ -n "$failed" ]; then
    printf '%s\n' "$failed" | while read -r _ seed ops; do
        head -1 "$out/fail-$seed.log" >&2
        if ! grep -qE "^$seed $ops( |$)" Tests/fsx-regressions.txt; then
            echo "$seed $ops  # $(date +%F): $(head -1 "$out/fail-$seed.log" | cut -c1-120)" >> Tests/fsx-regressions.txt
        fi
    done
    echo "fsx-soak: FAILED; seeds added to Tests/fsx-regressions.txt, logs in $out/fail-*.log" >&2
    exit 1
fi
echo "fsx-soak: PASSED"
