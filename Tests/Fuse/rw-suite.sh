#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Read-write suite through a real mount (plan Step 10).
#
#   Tests/Fuse/rw-suite.sh [--runs N] [--backend kext|fskit] [--no-git] [--disk]
#
# --disk attaches each image as a disk and mounts /dev/diskN (the daemon opens /dev/rdiskN
# with its page cache, D-62), the way real drives are used.
#
# Each run: a fresh image, mounted read-write; cp -R, rsync, ditto and a big file, each
# checked by SHA-256 against the source; mv, case-only rename, rm -r, truncate, touch -t,
# chflags hidden and uchg; xattrs (no ._ files), ln -s and ln; a git clone with fsck; unmount; the harness probes CLEAN and ntfsfix -n passes;
# then a read-only remount and every hash, xattr and link checked again. No `._` AppleDouble
# file may appear anywhere (xattrs are native since Step 16).
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
products="$root/build/DerivedData/Build/Products/Debug"
fuse="$products/unlatch-fuse" engine="$products/unlatch-engine"
ntfsfix="$root/build/ntfs3g/bin/ntfsfix"
runs=1 backend=kext git=1 disk=0
while [ $# -gt 0 ]; do
    case "$1" in
        --runs) runs="$2"; shift 2 ;;
        --backend) backend="$2"; shift 2 ;;
        --no-git) git=0; shift ;;
        --disk) disk=1; shift ;;
        *) echo "usage: $0 [--runs N] [--backend kext|fskit] [--no-git] [--disk]" >&2; exit 64 ;;
    esac
done

work="$root/build/rw-suite"
src="$root/Engine/vendor/ntfs-3g"           # a real source tree: ~1,000 files, nested dirs
mkdir -p "$work"
if [ "$backend" = fskit ]; then mnt=/Volumes/UnlatchRW; else mnt="$work/mnt"; mkdir -p "$mnt"; fi

say() { printf '  %-44s' "$1"; }
ok() { echo ok; }
dev=""
detach() { [ -z "$dev" ] || hdiutil detach -quiet "$dev" 2>/dev/null || true; dev=""; }
# A system indexer can hold a fresh volume for a moment after heavy writes (EBUSY).
unmount() { for _ in 1 2 3 4 5 6 7 8 9 10; do umount "$mnt" 2>/dev/null && return 0; sleep 0.5; done; umount "$mnt"; }
die() { echo "FAIL: $*" >&2; mount | grep -q " on $mnt " && unmount; sleep 0.5; detach; exit 1; }
readdir_writer="$root/build/readdir-writer"
clang -Wall -Wextra -Werror -o "$readdir_writer" "$root/Tests/Fuse/readdir-writer.c" || exit 1

# SHA-256 of every file under a tree, by relative path, without AppleDouble files.
hashes() { (cd "$1" && find . -type f ! -name '._*' -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256); }

expect_same() {
    diff <(hashes "$1") <(hashes "$2") > "$work/diff.txt" || { head -20 "$work/diff.txt" >&2; die "$3 differs"; }
}

mount_rw() { "$fuse" --backend "$backend" "$@" "$vol" "$mnt" 2>/dev/null || die "mount $*"; }

head -c $((300 << 20)) /dev/urandom > "$work/big.bin"
src_hashes="$work/src.sha"
hashes "$src" > "$src_hashes"

for run in $(seq 1 "$runs"); do
    echo "rw-suite: run $run of $runs ($backend backend)"
    rm -f "$work/run.img"
    img=$(cd "$work" && "$root/Scripts/make-image.sh" run 2g RWSUITE | tail -1)
    # D-83: names stored decomposed, as Windows may store them (the harness stores names as given).
    win_nfd=$(printf 'e\314\201te\314\201-nfd')
    printf nfd | "$engine" put "$img" - "/$win_nfd.txt" >/dev/null || die "put $win_nfd.txt"
    printf nfd | "$engine" put "$img" - "/$win_nfd-2.txt" >/dev/null || die "put $win_nfd-2.txt"
    vol="$img"
    if [ "$disk" = 1 ]; then dev=$("$root/Scripts/attach-image.sh" "$img"); vol="/dev/$dev"; fi
    mount_rw
    mount | grep " on $mnt " | grep -q read-only && die "mounted read-only"

    # D-81: a second mount of the mounted volume, read-write or read-only, by the same path or
    # (with --disk) the image behind the device, is refused.
    say "decomposed names from Windows"
    ls "$mnt" | grep -qx "$win_nfd.txt" || die "$win_nfd.txt not listed"
    [ "$(cat "$mnt/$win_nfd.txt")" = nfd ] || die "read $win_nfd.txt"
    [ "$(cat "$mnt/été-nfd.txt")" = nfd ] || die "read $win_nfd.txt by its NFC name"
    mv "$mnt/$win_nfd.txt" "$mnt/renamed-nfd.txt" && [ "$(cat "$mnt/renamed-nfd.txt")" = nfd ] || die "rename $win_nfd.txt"
    rm "$mnt/renamed-nfd.txt" "$mnt/$win_nfd-2.txt" || die "rm $win_nfd-2.txt"
    [ -z "$(ls "$mnt" | grep nfd)" ] || die "nfd names left: $(ls "$mnt" | grep nfd)"
    ok

    say "second mount of the volume refused"
    mkdir -p "$work/mnt2"
    for args in "$vol" "-r $vol" "$img"; do
        # shellcheck disable=SC2086
        if "$fuse" --backend kext $args "$work/mnt2" 2>/dev/null; then umount "$work/mnt2"; die "second mount: $args"; fi
    done
    ok

    say "cp -R a source tree";    cp -R "$src" "$mnt/cp";                  expect_same "$src" "$mnt/cp" "cp -R"; ok
    say "rsync -a";               rsync -a "$src/" "$mnt/rsync/";          expect_same "$src" "$mnt/rsync" "rsync"; ok
    say "ditto";                  ditto "$src" "$mnt/ditto";               expect_same "$src" "$mnt/ditto" "ditto"; ok
    say "300 MiB file";           cp "$work/big.bin" "$mnt/big.bin"
    [ "$(shasum -a 256 < "$mnt/big.bin")" = "$(shasum -a 256 < "$work/big.bin")" ] || die "big file"; ok

    say "mv a tree, case-only rename"
    mv "$mnt/ditto" "$mnt/moved"
    mv "$mnt/moved/README" "$mnt/moved/readme"
    [ -f "$mnt/moved/readme" ] && [ "$(ls "$mnt/moved" | grep -c '^readme$')" = 1 ] || die "case-only rename"
    mv "$mnt/moved/readme" "$mnt/moved/README"
    expect_same "$src" "$mnt/moved" "moved tree"; ok

    say "rm -r a tree";           rm -r "$mnt/rsync"; [ ! -e "$mnt/rsync" ] || die "rm -r"; ok
    # D-82: deleting frees the space while mounted, not at unmount; an open file keeps it.
    say "rm frees space at once, open file at close"
    # Used is minus Available: macFUSE's FSKit backend reports 0 used (it does show Available).
    used() { df -k "$mnt" | awk 'NR==2 { print -$4 }'; }
    cp "$work/big.bin" "$mnt/free.bin"; u0=$(used); rm "$mnt/free.bin"; u1=$(used)
    [ $((u0 - u1)) -ge $((290 << 10)) ] || die "rm freed $((u0 - u1)) KiB of 300 MiB"
    exec 4> "$mnt/open.bin"; cat "$work/big.bin" >&4; rm "$mnt/open.bin"; u2=$(used)
    [ $((u2 - u1)) -ge $((290 << 10)) ] || { exec 4>&-; die "an open, removed file lost its space"; }
    exec 4>&-; sleep 0.5; u3=$(used)
    [ $((u2 - u3)) -ge $((290 << 10)) ] || die "close freed $((u2 - u3)) KiB of 300 MiB"
    ok
    say "truncate grow and shrink"
    printf 'abcdef' > "$mnt/t.txt"
    truncate -s 1000000 "$mnt/t.txt"; [ "$(stat -f %z "$mnt/t.txt")" = 1000000 ] || die "grow"
    [ "$(tail -c +7 "$mnt/t.txt" | tr -d '\0' | wc -c | tr -d ' ')" = 0 ] || die "grown tail not zero"
    truncate -s 3 "$mnt/t.txt"; [ "$(cat "$mnt/t.txt")" = abc ] || die "shrink"; ok
    say "touch -t, chflags hidden"
    touch -t 200102030405.06 "$mnt/t.txt"
    [ "$(stat -f %Sm -t %Y%m%d%H%M%S "$mnt/t.txt")" = 20010203040506 ] || die "touch -t"
    chflags hidden "$mnt/t.txt"; ls -lO "$mnt/t.txt" | grep -q hidden || die "chflags"; ok
    say "chflags uchg -> Windows read-only"
    printf 'locked' > "$mnt/locked.txt"; chflags uchg "$mnt/locked.txt"
    [ "$(stat -f %Sp "$mnt/locked.txt")" = "-r--r--r--" ] || die "uchg did not set read-only"
    chmod u+w "$mnt/locked.txt"; printf '!' >> "$mnt/locked.txt" || die "chmod u+w"
    chmod a-w "$mnt/locked.txt"; rm "$mnt/locked.txt" || die "rm of a read-only file"; ok
    say "xattrs, no ._ files"
    xattr -w com.example.note "a note" "$mnt/t.txt"
    xattr -w com.apple.metadata:_kMDItemUserTags tagdata "$mnt/t.txt"
    [ "$(xattr -p com.example.note "$mnt/t.txt")" = "a note" ] || die "xattr value"
    cp -p "$src/README" "$mnt/readme-p" || die "cp -p"
    ok
    say "ln -s, ln"
    ln -s t.txt "$mnt/sym"; ln -s ../t.txt "$mnt/cp/up"; ln "$mnt/t.txt" "$mnt/hard"
    ln -s cp/include "$mnt/dirlink"; ln -s /Volumes/elsewhere/x "$mnt/abslink"    # native dir; WSL
    [ "$(cat "$mnt/sym")" = abc ] && [ "$(cat "$mnt/cp/up")" = abc ] || die "symlink targets"
    [ -d "$mnt/dirlink/ntfs-3g" ] && [ "$(readlink "$mnt/abslink")" = /Volumes/elsewhere/x ] || die "directory and absolute symlinks"
    rm "$mnt/dirlink"; ln -s cp/include "$mnt/dirlink"         # a directory symlink unlinks like a link
    [ "$(stat -f %l "$mnt/t.txt")" = 2 ] || die "nlink"; ok
    say "names: NFC/NFD, emoji, 255, reserved"
    mkdir "$mnt/names"
    nfd=$(printf 'cafe\xcc\x81.txt'); nfc=$(printf 'caf\xc3\xa9.txt')
    echo nfd > "$mnt/names/$nfd"
    [ "$(cat "$mnt/names/$nfc")" = nfd ] || die "NFD name not found by its NFC form"
    [ "$(ls "$mnt/names" | wc -l | tr -d ' ')" = 1 ] || die "NFD and NFC made two files"
    echo e > "$mnt/names/$(printf '\xf0\x9f\x93\x81 emoji \xe2\x9c\x85')" || die "emoji name"
    long=$(printf 'n%.0s' $(seq 1 255)); echo l > "$mnt/names/$long" || die "255-char name"
    ! echo x 2> /dev/null > "$mnt/names/${long}n" || die "256-char name accepted"
    for bad in CON aux.txt 'trail.' 'trail ' 'a:b' 'q?' 'lt<'; do
        ! touch "$mnt/names/$bad" 2> /dev/null || die "Windows-invalid name '$bad' accepted"
    done
    echo c > "$mnt/names/Case.txt"; mv "$mnt/names/Case.txt" "$mnt/names/case.TXT"
    ls "$mnt/names" | grep -qx 'case.TXT' || die "case-only rename"
    rm -r "$mnt/names"; ok

    say "readdir with a concurrent writer"
    mkdir "$mnt/rdw"; "$readdir_writer" "$mnt/rdw" 2000 > "$work/rdw.txt" 2>&1 || { cat "$work/rdw.txt" >&2; die "readdir-writer"; }
    rm -r "$mnt/rdw"; ok

    say "unlink while open"
    exec 3< "$mnt/big.bin"; rm "$mnt/big.bin"
    [ "$(shasum -a 256 <&3)" = "$(shasum -a 256 < "$work/big.bin")" ] || die "read after unlink"; exec 3<&-; ok

    if [ "$git" = 1 ]; then
    say "git clone, fsck"
    GIT_LFS_SKIP_SMUDGE=1 git clone -q --no-local "$root" "$mnt/clone" 2>/dev/null || die "git clone"
    git -C "$mnt/clone" fsck --no-progress --strict > "$work/fsck.txt" 2>&1 || { cat "$work/fsck.txt" >&2; die "git fsck"; }
    [ -z "$(GIT_LFS_SKIP_SMUDGE=1 git -C "$mnt/clone" status --porcelain | grep -v '^.. Tests/Images/' || true)" ] || die "git status not clean"
    ok
    fi

    say "no ._ files anywhere"
    [ -z "$(find "$mnt" -name '._*' | head -1)" ] || die "AppleDouble files: $(find "$mnt" -name '._*' | head -3)"; ok
    say "unmount, probe CLEAN, ntfsfix -n"
    cp_snapshot="$work/cp.sha"; (cd "$mnt/cp" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256) > "$cp_snapshot"
    unmount
    "$engine" probe "$vol" | grep -q '^state: *clean' || die "probe after unmount: $("$engine" probe "$vol" | head -1)"
    "$ntfsfix" -n "$vol" > "$work/ntfsfix.txt" 2>&1 || { cat "$work/ntfsfix.txt" >&2; die "ntfsfix -n"; }
    ok

    say "read-only remount, hashes again"
    mount_rw -r
    diff "$cp_snapshot" <(cd "$mnt/cp" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256) > /dev/null || die "cp tree after remount"
    [ "$(xattr -p com.example.note "$mnt/t.txt")" = "a note" ] || die "xattr after remount"
    [ "$(readlink "$mnt/sym")" = t.txt ] && [ "$(stat -f %l "$mnt/hard")" = 2 ] || die "links after remount"
    [ "$(readlink "$mnt/dirlink")" = cp/include ] && [ -d "$mnt/dirlink/ntfs-3g" ] || die "directory symlink after remount"
    expect_same "$src" "$mnt/moved" "moved tree after remount"
    [ "$(cat "$mnt/t.txt")" = abc ] || die "t.txt after remount"
    [ ! -e "$mnt/big.bin" ] || die "unlinked file came back"
    unmount; ok
    [ -z "$dev" ] || { sleep 0.5; detach; }
done
echo "rw-suite: $runs run(s) passed"
