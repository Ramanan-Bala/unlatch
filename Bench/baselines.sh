#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs Bench/run.sh on Unlatch and the baselines, each on its own 4 GiB image on the internal
# SSD, attached as a disk (plan Step 11). Image-backed: the images live in the host's page
# cache, so this measures each file system stack's overhead, not a drive. The disposable-SSD
# run of the plan uses the same script with --disk.
#
#   Bench/baselines.sh [--runs N] [kind ...]     kinds: apfs exfat ntfs3g unlatch-file unlatch
#                                                 (default: all)
#
# Stock ntfs-3g refuses block devices unless run as root ("unprivileged user can not mount
# NTFS block devices using the external FUSE library"), so it mounts the image file; the
# unlatch-file run does the same for a like-for-like comparison, and unlatch uses the disk.
#   Bench/baselines.sh remount <kind>             used by run.sh between write and cold read
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$root/build/bench"
products="$root/build/DerivedData/Build/Products/Debug"
mnt_fuse="$work/m"
mkdir -p "$work" "$mnt_fuse"

dev_of() { cat "$work/$1.dev"; }
mount_point() { case "$1" in apfs) echo /Volumes/BAPFS ;; exfat) echo /Volumes/BEXFAT ;; *) echo "$mnt_fuse" ;; esac; }

fuse_mount() {          # kind
    local disk; disk=$(dev_of "$1")
    case "$1" in
        unlatch) "$products/unlatch-fuse" "/dev/$disk" "$mnt_fuse" 2>/dev/null ;;
        unlatch-file) "$products/unlatch-fuse" "$work/$1.img" "$mnt_fuse" 2>/dev/null ;;
        ntfs3g)
            # ntfs-3g dies after forking on macOS (like any fork without exec), so no_detach.
            ("$root/build/ntfs3g-driver/bin/ntfs-3g" -o no_detach "$work/$1.img" "$mnt_fuse" > "$work/ntfs3g.log" 2>&1 &)
            for _ in $(seq 1 100); do mount | grep -q " on $mnt_fuse " && return 0; sleep 0.1; done
            echo "ntfs-3g did not mount" >&2; return 1 ;;
    esac
}

setup() {
    local kind="$1" img="$work/$1.img" disk
    rm -f "$img" "$img.dmg"
    case "$kind" in
        apfs|exfat)
            local fs=APFS name=BAPFS
            [ "$kind" = exfat ] && fs=ExFAT name=BEXFAT
            hdiutil create -quiet -size 4g -layout NONE -fs "$fs" -volname "$name" "$img.dmg"
            disk=$(hdiutil attach -nobrowse "$img.dmg" | awk 'NR == 1 {sub("/dev/", "", $1); print $1}')
            # APFS mounts a synthesized volume device, not the physical store: remount that.
            diskutil info "$(mount_point "$kind")" | awk '/Device Identifier:/ {print $3}' > "$work/$kind.vol" ;;
        ntfs3g|unlatch|unlatch-file)
            (cd "$work" && "$root/Scripts/make-image.sh" "$kind" 4g BENCH > /dev/null)
            # File-backed setups stay unattached: an attached image is locked against ntfs-3g.
            disk=""
            [ "$kind" = unlatch ] && disk=$("$root/Scripts/attach-image.sh" "$img" 2>/dev/null)
            echo "$disk" > "$work/$kind.dev"
            fuse_mount "$kind" ;;
    esac
    echo "$disk" > "$work/$kind.dev"
}

remount() {
    local kind="$1" mnt; mnt=$(mount_point "$kind")
    case "$kind" in
        apfs|exfat) diskutil unmount "$mnt" > /dev/null && diskutil mount "$(cat "$work/$kind.vol")" > /dev/null ;;
        *) umount "$mnt" && sleep 0.5 && fuse_mount "$kind" ;;
    esac
}

teardown() {
    local kind="$1" mnt; mnt=$(mount_point "$kind")
    case "$kind" in
        apfs|exfat) hdiutil detach -quiet "$(dev_of "$kind")" ;;
        *) umount "$mnt" 2>/dev/null || true; sleep 0.5
           [ -z "$(dev_of "$kind")" ] || hdiutil detach -quiet "$(dev_of "$kind")" 2>/dev/null || true ;;
    esac
    rm -f "$work/$kind.img" "$work/$kind.img.dmg" "$work/$kind.dev" "$work/$kind.vol"
}

if [ "${1:-}" = remount ]; then remount "$2"; exit; fi
runs=5
[ "${1:-}" = --runs ] && { runs="$2"; shift 2; }
kinds=("$@"); [ ${#kinds[@]} -gt 0 ] || kinds=(apfs exfat ntfs3g unlatch-file unlatch)
for kind in "${kinds[@]}"; do
    setup "$kind"
    dev=(); [ -z "$(dev_of "$kind")" ] || dev=(--device "r$(dev_of "$kind")")
    "$root/Bench/run.sh" "$(mount_point "$kind")" "$kind" --runs "$runs" ${dev[@]+"${dev[@]}"} \
        --remount "'$root/Bench/baselines.sh' remount $kind" || { teardown "$kind"; exit 1; }
    teardown "$kind"
done
