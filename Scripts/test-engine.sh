#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the engine and its C tests, prepares test images under build/test-engine,
# runs the tests, the harness smoke test and a leak check.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
out="$root/build/test-engine"
bin="$root/build/ntfs3g/bin"
products="$root/build/DerivedData/Build/Products/Debug"

[ -f build/ntfs3g/lib/libntfs-3g.a ] || Scripts/build-ntfs3g.sh
[ -d Unlatch.xcodeproj ] || xcodegen generate >/dev/null

echo "== build engine"
rm -rf "$out"
mkdir -p "$out"
if ! xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build > "$out/xcodebuild.log" 2>&1; then
    grep -E "error:" "$out/xcodebuild.log" >&2 || tail -30 "$out/xcodebuild.log" >&2
    echo "test-engine: engine build failed (log: $out/xcodebuild.log)" >&2
    exit 1
fi
cflags=(-std=c11 -Wall -Wextra -Werror -g -arch arm64 -mmacosx-version-min=27.0 -IEngine/include -IEngineTool
        -IEngine/src/fixtures)
clang "${cflags[@]}" -o "$out/test_engine" Tests/Engine/test_engine.c EngineTool/file_io.c EngineTool/walk.c \
    "$products/libUnlatchEngine.a"
clang "${cflags[@]}" -o "$out/set_dirty" Tests/Engine/set_dirty.c
# Fixture builder: test tooling that uses libntfs-3g directly (D-20).
clang "${cflags[@]}" -DHAVE_CONFIG_H=1 -Ibuild/ntfs3g/config -isystem build/ntfs3g/include \
    -o "$out/mkfixture" Engine/src/fixtures/mkfixture.c build/ntfs3g/lib/libntfs-3g.a

echo "== prepare images"
cd "$out"
"$root/Scripts/make-image.sh" clean 512m T1 >/dev/null
cp -c clean.img dirty.img
"$out/set_dirty" dirty.img
cp -c clean.img zeroboot.img
dd if=/dev/zero of=zeroboot.img bs=512 count=1 conv=notrunc status=none
cp -c clean.img truncated.img
truncate -s 256m truncated.img
cp -c clean.img hiber.img
{ printf 'hibr'; head -c 8188 /dev/zero; } > hiberfil.sys
"$bin/ntfscp" -q hiber.img hiberfil.sys hiberfil.sys
cp -c clean.img rw.img
"$root/Scripts/make-image.sh" small 32m SMALL >/dev/null
cp -c clean.img leak.img
"$root/Scripts/make-image.sh" read 8g READ >/dev/null
"$out/mkfixture" read.img <<'FIXTURE'
mkdir /dir1
mkdir /dir1/dir2
file /dir1/dir2/deep.bin 100000 7
file /ReadMe.TXT 5000 1
file /café.txt 12 2
file /small.txt 100 5
flags /small.txt hidden
sparse /sparse.bin 3221225472 2147483648 1048576 3
uninit /uninit.bin 1048576 4096 4
mkdir /big
many /big 10000
FIXTURE
# Windows reparse points as Windows writes them (Step 16): symlinks and a junction.
"$root/Scripts/make-image.sh" reparse 32m REPARSE >/dev/null
"$out/mkfixture" reparse.img <<'FIXTURE'
mkdir /Users
mkdir /Users/Public
file /Users/Public/hello.txt 100 1
file /rel.lnk 0 1
reparse /rel.lnk symlink-rel Users\Public\hello.txt
file /abs.lnk 0 1
reparse /abs.lnk symlink-abs C:\Users\Public\hello.txt
mkdir /Docs
reparse /Docs junction C:\Users\Public
file /other.lnk 0 1
reparse /other.lnk symlink-abs D:\elsewhere\x.txt
mkdir /damaged
file /damaged/a.txt 10 1
file /damaged/broken.txt 10 2
file /damaged/c.txt 10 3
zero-record /damaged/broken.txt
FIXTURE
if [ "${UNLATCH_SKIP_BIG:-0}" != 1 ]; then
    "$root/Scripts/make-image.sh" big 5g BIG >/dev/null
    echo "file /4g.bin 4294967296 9" | "$out/mkfixture" big.img
fi

echo "== unit tests"
"$out/test_engine" "$out"

echo "== harness"
"$products/unlatch-engine" probe clean.img > probe.txt
cat probe.txt
grep -q '^state: *clean$' probe.txt
grep -q '^label: *T1$' probe.txt
grep -q '^total_bytes: *[1-9]' probe.txt
"$products/unlatch-engine" probe dirty.img | grep -q '^state: *dirty$'
"$products/unlatch-engine" probe zeroboot.img | grep -q '^state: *not_ntfs$'
"$products/unlatch-engine" info --rw clean.img | grep -q '^mode: *read-write$'
echo "ok   harness probe/info"
eng="$products/unlatch-engine"
"$eng" ls read.img / | grep -q ' ReadMe.TXT$'
"$eng" stat read.img /readme.txt | grep -q '^size: *5000$'
"$eng" cat read.img /dir1/dir2/deep.bin | cmp - <("$out/mkfixture" gen 100000 7)
"$eng" tree read.img /dir1 | grep -q '^  deep.bin$'
"$eng" ls --stats read.img /big 2> stats.txt > /dev/null
grep -q '^entries: 10000 inode_opens: 10001$' stats.txt
echo "ok   harness ls/stat/cat/tree; /big: $(cat stats.txt)"
if [ "${UNLATCH_SKIP_BIG:-0}" != 1 ]; then
    want="$("$out/mkfixture" gen 4294967296 9 | shasum -a 256 | cut -d' ' -f1)"
    start=$SECONDS
    got="$("$eng" cat big.img /4g.bin | shasum -a 256 | cut -d' ' -f1)"
    [ "$got" = "$want" ] || { echo "FAIL cat 4 GiB: $got != $want" >&2; exit 1; }
    echo "ok   cat of a 4 GiB file matches shasum ($((SECONDS - start)) s)"
    "$root/Scripts/make-image.sh" put 5g PUT >/dev/null
    want="$("$out/mkfixture" gen 4294967296 10 | shasum -a 256 | cut -d' ' -f1)"
    start=$SECONDS
    "$out/mkfixture" gen 4294967296 10 | "$eng" put put.img - /4g.bin
    put_s=$((SECONDS - start))
    start=$SECONDS
    got="$("$eng" get put.img /4g.bin - | shasum -a 256 | cut -d' ' -f1)"
    [ "$got" = "$want" ] || { echo "FAIL put/get 4 GiB: $got != $want" >&2; exit 1; }
    "$eng" probe put.img | grep -q '^state: *clean$' || { echo "FAIL put.img not clean" >&2; exit 1; }
    "$bin/ntfsfix" -n put.img > /dev/null || { echo "FAIL ntfsfix -n put.img" >&2; exit 1; }
    echo "ok   4 GiB put ($put_s s) and get ($((SECONDS - start)) s) round trip matches shasum; image CLEAN"
else
    echo "skip cat of a 4 GiB file (UNLATCH_SKIP_BIG=1)"
fi

echo "== leaks (100 read-write open/close)"
if leaks --atExit -- "$out/test_engine" leakloop leak.img > leaks.txt 2>&1; then
    grep -E "leaks for|total leaked" leaks.txt | head -2
    echo "ok   no leaks"
else
    cat leaks.txt >&2
    echo "FAIL leaks" >&2
    exit 1
fi
echo "== leaks (50 open, walk, read, readdir, close)"
if leaks --atExit -- "$out/test_engine" readloop read.img > leaks-read.txt 2>&1; then
    grep -E "leaks for" leaks-read.txt | head -1
    echo "ok   no leaks"
else
    cat leaks-read.txt >&2
    echo "FAIL leaks" >&2
    exit 1
fi

echo "== leaks (30 create, write, rename, remove on fresh clones)"
if leaks --atExit -- "$out/test_engine" writeloop "$out" > leaks-write.txt 2>&1; then
    grep -E "leaks for" leaks-write.txt | head -1
    echo "ok   no leaks"
else
    cat leaks-write.txt >&2
    echo "FAIL leaks" >&2
    exit 1
fi

echo "== written images: probe CLEAN, ntfsinfo, ntfsfix -n, no internal names left"
for w in w-*.img; do
    "$products/unlatch-engine" probe "$w" | grep -q '^state: *clean$' || { echo "FAIL $w not clean" >&2; exit 1; }
    "$bin/ntfsinfo" -m "$w" > /dev/null 2>&1 || { echo "FAIL ntfsinfo $w" >&2; exit 1; }
    "$bin/ntfsfix" -n "$w" > fix.txt 2>&1 || { cat fix.txt >&2; echo "FAIL ntfsfix -n $w" >&2; exit 1; }
    if "$bin/ntfsls" -R -a -s "$w" | grep -q '\.unlatch-'; then
        echo "FAIL $w has leftover .unlatch- names" >&2; exit 1
    fi
done
echo "ok   $(ls w-*.img | wc -l | tr -d ' ') written images"

echo "== fsx regressions"
"$root/Scripts/fsx-soak.sh" --regressions | tail -1

echo "== include boundary"
if grep -rn --include='*.c' --include='*.h' --include='*.swift' --include='*.m' -E '#\s*include\s*[<"]ntfs-3g/' \
    "$root" --exclude-dir=vendor --exclude-dir=build --exclude-dir=.build \
    | grep -v "^$root/Engine/src/"; then
    echo "FAIL ntfs-3g headers included outside Engine/src" >&2
    exit 1
fi
echo "ok   ntfs-3g headers only in Engine/src"

echo "test-engine: PASSED"
