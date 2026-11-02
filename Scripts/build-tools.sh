#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds unlatch-fuse and the privileged helper with clang alone, without the Xcode project
# (plan Step 18): the Homebrew formula uses it, because xcodebuild's package resolution cannot
# run inside Homebrew's build sandbox. Same sources and flags as project.yml's targets.
#
#   Scripts/build-tools.sh <outdir>        needs build/ntfs3g (Scripts/build-ntfs3g.sh) and macFUSE
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?usage: $0 <outdir>}"
fuse="${MACFUSE_PREFIX:-/usr/local}"
cd "$root"
[ -f build/ntfs3g/lib/libntfs-3g.a ] || { echo "build-tools: run Scripts/build-ntfs3g.sh first" >&2; exit 1; }
[ -f "$fuse/include/fuse3/fuse_lowlevel.h" ] || { echo "build-tools: macFUSE headers missing in $fuse" >&2; exit 1; }
mkdir -p "$out/obj"
cflags=(-std=c11 -O2 -arch arm64 -mmacosx-version-min=27.0 -Wall -Wextra -Werror)

# The engine: Engine/src without the test fixtures, against libntfs-3g's own config.h (D-13).
objs=()
for c in Engine/src/*.c; do
    o="$out/obj/$(basename "$c" .c).o"
    clang "${cflags[@]}" -DHAVE_CONFIG_H=1 -IEngine/include -Ibuild/ntfs3g/config -isystem build/ntfs3g/include \
        -c "$c" -o "$o"
    objs+=("$o")
done
clang "${cflags[@]}" -IEngine/include -IEngineTool -isystem "$fuse/include/fuse3" \
    Fuse/*.c EngineTool/file_io.c "${objs[@]}" build/ntfs3g/lib/libntfs-3g.a \
    -L"$fuse/lib" -lfuse3 -framework CoreFoundation -framework DiskArbitration -o "$out/unlatch-fuse"
clang "${cflags[@]}" Helper/main.c Helper/policy.c -framework CoreFoundation -framework DiskArbitration \
    -o "$out/io.github.ramanan-bala.unlatch.helper"
rm -rf "$out/obj"
echo "$out/unlatch-fuse"
echo "$out/io.github.ramanan-bala.unlatch.helper"
