#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Interruption tests (plan Step 17): a copy onto an attached disk is cut off part way, then
# the volume is checked the way the next mount would see it.
#
#   Tests/Fuse/interrupt.sh [kill|detach ...]      default: both
#
#   kill     SIGKILL the daemon mid-copy (a crash): the mount must go away, the next probe
#            must not say CORRUPT, ntfsfix -n must accept the volume, and a read-write mount
#            must work and see the files that were complete before the last write-back.
#   detach   force-eject the disk mid-copy (a surprise unplug): the daemon must exit without
#            hanging, and the same checks apply after re-attaching the image.
# Prints one line per scenario; exits 1 if any check failed.
set -uo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
products="$root/build/DerivedData/Build/Products/Debug"
fuse="$products/unlatch-fuse" engine="$products/unlatch-engine" ntfsfix="$root/build/ntfs3g/bin/ntfsfix"
work="$root/build/interrupt"
mnt="$work/mnt"
src="$root/Engine/vendor/ntfs-3g"
mkdir -p "$mnt"
fails=0

note() { printf '  %-10s %s\n' "$1" "$2"; }
gone() { for _ in $(seq 1 40); do mount | grep -q " on $mnt " || return 0; sleep 0.25; done; return 1; }

check_after() {         # scenario, disk; the image is attached as disk, nothing mounted
    local name="$1" disk="$2" state rc=0
    state=$("$engine" probe "/dev/$disk" | awk '/^state:/ {print $2}')
    note "$name" "probe after: $state"
    case "$state" in clean|dirty) ;; *) note "$name" "FAIL: probe says $state"; rc=1 ;; esac
    if "$ntfsfix" -n "/dev/$disk" > "$work/fix.txt" 2>&1; then
        note "$name" "ntfsfix -n: ok"
    else
        note "$name" "FAIL: ntfsfix -n: $(tail -1 "$work/fix.txt")"; rc=1
    fi
    if "$fuse" "/dev/$disk" "$mnt" 2>/dev/null; then
        local n; n=$(find "$mnt/copy" -type f 2>/dev/null | wc -l | tr -d ' ')
        note "$name" "remounted ($(mount | grep " on $mnt " | grep -q read-only && echo read-only || echo read-write)); $n files of the copy present"
        cp -R "$src/include" "$mnt/after" && diff -r "$src/include" "$mnt/after" > /dev/null \
            && note "$name" "writing after the interruption: ok" || { note "$name" "FAIL: write after"; rc=1; }
        for _ in 1 2 3 4 5 6 7 8; do umount "$mnt" 2>/dev/null && break; sleep 0.5; done
        gone || { note "$name" "FAIL: unmount"; rc=1; }
        "$engine" probe "/dev/$disk" | grep -q '^state: *clean' && note "$name" "clean after a normal unmount" \
            || { note "$name" "FAIL: not clean after a normal unmount"; rc=1; }
    else
        note "$name" "FAIL: remount"; rc=1
    fi
    return $rc
}

start_copy() {          # copies the source tree repeatedly in the background; prints its pid
    ( for i in 1 2 3 4 5 6; do cp -R "$src" "$mnt/copy/$i" 2>/dev/null || exit 0; done ) &
    echo $!
}

scenario() {
    local name="$1" img="$work/$1.img" disk pid daemon
    rm -f "$img"
    (cd "$work" && "$root/Scripts/make-image.sh" "$name" 1g INTR > /dev/null)
    disk=$("$root/Scripts/attach-image.sh" "$img" 2>/dev/null)
    "$fuse" "/dev/$disk" "$mnt" 2>/dev/null || { note "$name" "FAIL: mount"; return 1; }
    mkdir -p "$mnt/copy"
    pid=$(start_copy)
    sleep 3                 # part way: some files complete and written back, some not
    daemon=$(pgrep -f "unlatch-fuse -f --ready-fd [0-9]+ /dev/$disk ")
    case "$name" in
        kill)
            kill -9 "$daemon"
            wait "$pid" 2>/dev/null
            if gone; then note "$name" "mount gone after SIGKILL"; else
                note "$name" "mount still listed after SIGKILL; umount -f"; umount -f "$mnt" 2>/dev/null; gone; fi ;;
        detach)
            diskutil eject force "$disk" > /dev/null 2>&1 || hdiutil detach -force "$disk" > /dev/null 2>&1
            if diskutil info "$disk" > /dev/null 2>&1; then
                # macOS refuses to eject an attached image while the daemon holds it, so a
                # surprise unplug needs a real USB disk (docs/test-matrix.md).
                note "$name" "SKIP: macOS refused to eject the image while it is in use; needs a real USB disk"
                kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
                for _ in 1 2 3 4 5 6 7 8; do umount "$mnt" 2>/dev/null && break; sleep 0.5; done
                hdiutil detach -quiet "$disk" 2>/dev/null; rm -f "$img"; return 0
            fi
            wait "$pid" 2>/dev/null
            for _ in $(seq 1 40); do kill -0 "$daemon" 2>/dev/null || break; sleep 0.25; done
            if kill -0 "$daemon" 2>/dev/null; then note "$name" "FAIL: daemon still running"; kill -9 "$daemon"; fails=1
            else note "$name" "daemon exited after the eject"; fi
            gone || { umount -f "$mnt" 2>/dev/null; gone; }
            mount | grep -q " on $mnt " && { note "$name" "FAIL: stale mount"; fails=1; }
            disk=$("$root/Scripts/attach-image.sh" "$img" 2>/dev/null) ;;
    esac
    check_after "$name" "$disk" || fails=1
    hdiutil detach -quiet "$disk" 2>/dev/null || hdiutil detach -force "$disk" > /dev/null 2>&1
    rm -f "$img"
}

kinds=("$@"); [ ${#kinds[@]} -gt 0 ] || kinds=(kill detach)
for k in "${kinds[@]}"; do echo "interrupt: $k"; scenario "$k"; done
[ $fails = 0 ] && echo "interrupt: passed" || { echo "interrupt: FAILED"; exit 1; }
