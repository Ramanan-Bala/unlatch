#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Compares a mounted NTFS volume with what the harness reads from the same image (plan Step 08).
#
#   Tests/Fuse/check-readonly.sh <image> <mountpoint>      (mounted with unlatch-fuse -r)
#
# Every directory: names, types and sizes match `unlatch-engine ls`. Every file: SHA-256
# through the mount matches `unlatch-engine cat`; every link reads back a target. Then df totals match `unlatch-engine info`,
# and writes are refused.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
engine="${UNLATCH_ENGINE:-$root/build/DerivedData/Build/Products/Debug/unlatch-engine}"
img="$1" mnt="$2"
fail=0 dirs=0 files=0

bad() { echo "FAIL: $*" >&2; fail=$((fail + 1)); }

# One line per entry, "type size name", sorted, for the harness and for the mount.
harness_ls() { "$engine" ls "$img" "$1" | awk '{t=$1; s=$2; $1=$2=""; sub(/^  /,""); print t, s, $0}' | LC_ALL=C sort; }
mount_ls() {
    local d="$mnt$1" f
    for f in "$d"/* "$d"/.[!.]* "$d"/..?*; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        if [ -L "$f" ]; then
            printf 'l 0 %s\n' "${f##*/}"   # the harness lists links with size 0 (Step 16)
        elif [ -d "$f" ]; then
            printf 'd %s %s\n' "$(stat -f %z "$f")" "${f##*/}"
        else
            printf -- '- %s %s\n' "$(stat -f %z "$f")" "${f##*/}"
        fi
    done | LC_ALL=C sort
}

walk() {
    local rel="$1" line type name
    dirs=$((dirs + 1))
    if ! diff <(harness_ls "${rel:-/}") <(mount_ls "$rel") >/dev/null; then
        bad "listing of ${rel:-/} differs:"
        diff <(harness_ls "${rel:-/}") <(mount_ls "$rel") | head -10 >&2 || true
    fi
    while IFS= read -r line; do
        type="${line%% *}"
        name="${line#* }"; name="${name#* }"
        if [ "$type" = d ]; then
            walk "$rel/$name"
        elif [ "$type" = l ]; then
            [ -n "$(readlink "$mnt$rel/$name")" ] || bad "link $rel/$name has no target"
        else
            files=$((files + 1))
            a=$("$engine" cat "$img" "$rel/$name" | shasum -a 256 | cut -d' ' -f1)
            b=$(shasum -a 256 < "$mnt$rel/$name" | cut -d' ' -f1)
            [ "$a" = "$b" ] || bad "content of $rel/$name differs"
        fi
    done < <(harness_ls "${rel:-/}")
}

walk ""

# Space: df's 1 KiB blocks against the engine's byte counts.
total=$("$engine" info "$img" | awk '/^total_bytes:/ {print $2}')
free=$("$engine" info "$img" | awk '/^free_bytes:/ {print $2}')
read -r blocks avail < <(df -k "$mnt" | awk 'NR == 2 {print $2, $4}')
[ "$blocks" -eq $((total / 1024)) ] || bad "df total $blocks KiB, engine $((total / 1024))"
[ "$avail" -eq $((free / 1024)) ] || bad "df available $avail KiB, engine $((free / 1024))"

# Read-only: nothing may change. A clean volume mounts read-write unless -r was given.
mount | grep " on $(cd "$mnt" && pwd) " | grep -q read-only || bad "mount is not read-only (mount with unlatch-fuse -r)"
touch "$mnt/.unlatch-check" 2>/dev/null && bad "create succeeded on a read-only mount"
mkdir "$mnt/.unlatch-check-d" 2>/dev/null && bad "mkdir succeeded on a read-only mount"

echo "check-readonly: $dirs directories, $files files compared, $fail failures"
[ "$fail" -eq 0 ]
