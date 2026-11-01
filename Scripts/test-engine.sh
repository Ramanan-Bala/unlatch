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
cflags=(-std=c11 -Wall -Wextra -Werror -g -arch arm64 -mmacosx-version-min=27.0 -IEngine/include -IEngineTool)
clang "${cflags[@]}" -o "$out/test_engine" Tests/Engine/test_engine.c EngineTool/file_io.c \
    "$products/libUnlatchEngine.a"
clang "${cflags[@]}" -o "$out/set_dirty" Tests/Engine/set_dirty.c

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
cp -c clean.img leak.img

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

echo "== leaks (100 read-write open/close)"
if leaks --atExit -- "$out/test_engine" leakloop leak.img > leaks.txt 2>&1; then
    grep -E "leaks for|total leaked" leaks.txt | head -2
    echo "ok   no leaks"
else
    cat leaks.txt >&2
    echo "FAIL leaks" >&2
    exit 1
fi

echo "== include boundary"
if grep -rn --include='*.c' --include='*.h' --include='*.swift' --include='*.m' -E '#\s*include\s*[<"]ntfs-3g/' \
    "$root" --exclude-dir=vendor --exclude-dir=build --exclude-dir=.build \
    | grep -v "^$root/Engine/src/"; then
    echo "FAIL ntfs-3g headers included outside Engine/src" >&2
    exit 1
fi
echo "ok   ntfs-3g headers only in Engine/src"

echo "test-engine: PASSED"
