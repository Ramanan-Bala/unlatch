#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds and runs the engine's libFuzzer targets (ASan + UBSan) over mutated NTFS images.
#
#   Scripts/fuzz.sh [--time SECONDS] [target ...]   fuzz each target (default 60 s each)
#   Scripts/fuzz.sh --regress                       only replay Tests/Fuzz/crashes/*
#   Scripts/fuzz.sh --check-asan                    exit 0 if an ASan program starts within 30 s
#
# Saved crashes are replayed first. A new crash is copied to Tests/Fuzz/crashes/<target>/
# and the script fails. Needs Homebrew LLVM (Apple clang has no libFuzzer runtime).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
secs=60 regress=0 check_asan=0 targets=()
while [ $# -gt 0 ]; do
    case "$1" in
        --time) secs="$2"; shift 2 ;;
        --regress) regress=1; shift ;;
        --check-asan) check_asan=1; shift ;;
        fuzz_*) targets+=("$1"); shift ;;
        *) echo "usage: $0 [--time S] [--regress] [fuzz_probe|fuzz_open_ls ...]" >&2; exit 64 ;;
    esac
done
[ ${#targets[@]} -gt 0 ] || targets=(fuzz_probe fuzz_open_ls)

out="$root/build/fuzz"
cc=""
for p in "${LLVM_PREFIX:-}" "$(brew --prefix llvm 2>/dev/null)" "$(brew --prefix llvm@20 2>/dev/null)"; do
    [ -n "$p" ] && [ -x "$p/bin/clang" ] && { cc="$p/bin/clang"; break; }
done
[ -n "$cc" ] || { echo "fuzz: needs Homebrew LLVM (brew install llvm) or LLVM_PREFIX" >&2; exit 1; }
if [ "$check_asan" = 1 ]; then
    # ASan programs from Homebrew LLVM have hung at startup on the CI runner (D-35).
    mkdir -p build/fuzz
    echo 'int main(void) { return 0; }' > build/fuzz/asan-check.c
    "$cc" -fsanitize=address -isysroot "$(xcrun --sdk macosx --show-sdk-path)" -o build/fuzz/asan-check build/fuzz/asan-check.c
    exec perl -e 'alarm 30; exec @ARGV' build/fuzz/asan-check
fi
[ -f build/ntfs3g-fuzz/lib/libntfs-3g.a ] || Scripts/build-ntfs3g.sh --fuzz
[ -f build/ntfs3g/lib/libntfs-3g.a ] || Scripts/build-ntfs3g.sh
mkdir -p "$out/seeds" "$out/artifacts"

sdk="$(xcrun --sdk macosx --show-sdk-path)"
# alignment: packed on-disk structs; shift-base: libntfs-3g sign-extends with (s8)x << 8 by design (D-33).
san=(-fsanitize=address,undefined -fno-sanitize=alignment,shift-base -fno-sanitize-recover=undefined)
cflags=(-std=c11 -g -O1 -fno-omit-frame-pointer -arch arm64 -mmacosx-version-min=27.0 -isysroot "$sdk"
        -Wall -Wextra -Werror -DHAVE_CONFIG_H=1 -IEngine/include -Ibuild/ntfs3g-fuzz/config
        -isystem build/ntfs3g-fuzz/include)
engine_src=(Engine/src/*.c)
for t in "${targets[@]}"; do
    echo "fuzz: building $t"
    "$cc" "${cflags[@]}" "${san[@]}" -fsanitize=fuzzer -o "$out/$t" "Tests/Fuzz/$t.c" "${engine_src[@]}" \
        build/ntfs3g-fuzz/lib/libntfs-3g.a
done

# Seeds: real images in the fuzz "sector list" format (Tests/Fuzz/README.md).
echo "fuzz: building seeds"
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -o "$out/mkseed" Tests/Fuzz/mkseed.c
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -o "$out/set_dirty" Tests/Engine/set_dirty.c
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -DHAVE_CONFIG_H=1 -Ibuild/ntfs3g/config -isystem build/ntfs3g/include \
    -IEngine/src/fixtures -o "$out/mkfixture" Engine/src/fixtures/mkfixture.c build/ntfs3g/lib/libntfs-3g.a
(
    [ -n "${CI:-}" ] && set -x        # the CI runner once hung here; show where
    cd "$out/seeds"
    rm -f ./*.img ./*.seed
    "$root/Scripts/make-image.sh" fresh 2m FRESH > /dev/null
    "$root/Scripts/make-image.sh" full 4m FULL > /dev/null
    "$out/mkfixture" full.img <<'FIXTURE'
mkdir /d
mkdir /d/e
file /d/e/deep.bin 70000 1
file /small.txt 300 2
file /ReadMe.TXT 5000 3
file /café.txt 20 4
flags /small.txt hidden
sparse /sparse.bin 1048576 524288 8192 5
uninit /uninit.bin 65536 4096 6
mkdir /many
many /many 300
FIXTURE
    cp fresh.img dirty.img && "$out/set_dirty" dirty.img
    cp fresh.img hiber.img
    { printf 'hibr'; head -c 4092 /dev/zero; } > hiberfil.sys
    "$root/build/ntfs3g/bin/ntfscp" -q hiber.img hiberfil.sys hiberfil.sys
    for i in *.img; do "$out/mkseed" "$i" "${i%.img}.seed"; done
)

rc=0
for t in "${targets[@]}"; do
    crashes=(Tests/Fuzz/crashes/$t/*)
    for c in "${crashes[@]}"; do
        [ -f "$c" ] && [ "$(basename "$c")" != .gitkeep ] || continue
        "$out/$t" "$c" > "$out/$t-regress.log" 2>&1 || { echo "FAIL $t regression $c" >&2; tail -30 "$out/$t-regress.log" >&2; rc=1; }
    done
    [ "$regress" = 1 ] && continue
    corpus="$out/corpus/$t"
    mkdir -p "$corpus"
    cp "$out"/seeds/*.seed "$corpus/"
    rm -f "$out/artifacts/$t-"*
    echo "fuzz: $t for $secs s"
    if "$out/$t" "$corpus" -max_total_time="$secs" -max_len=$((4 + 516 * 16384)) -timeout=20 \
        -rss_limit_mb=4096 -artifact_prefix="$out/artifacts/$t-" -print_final_stats=1 > "$out/$t.log" 2>&1; then
        grep -E "stat::number_of_executed_units|stat::peak_rss_mb|cov:" "$out/$t.log" | tail -3 | sed 's/^/  /'
    else
        rc=1
        tail -40 "$out/$t.log" >&2
        mkdir -p "Tests/Fuzz/crashes/$t"
        for a in "$out/artifacts/$t-"*; do
            [ -f "$a" ] && cp "$a" "Tests/Fuzz/crashes/$t/" && echo "fuzz: saved $(basename "$a") to Tests/Fuzz/crashes/$t/" >&2
        done
    fi
done
[ "$rc" = 0 ] && echo "fuzz: PASSED" || { echo "fuzz: FAILED" >&2; exit 1; }
