#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests Unlatch on a real, disposable NTFS disk: everything the image-based suites cannot do.
#
#   Scripts/test-real-disk.sh <disk> [--yes] [--quick] [--no-physical] [--fill] [--bench] [--cut] [--only s1,s2]
#
#   <disk>          the NTFS volume: BSD name (disk6s2), /dev path or label
#   --yes           do not ask to confirm the disk
#   --quick         smaller files, one integrity snapshot (about 20 min instead of 1-2 h on USB 2.0)
#   --no-physical   skip the steps that need you at the Mac (replug, pull out, sleep)
#   --fill          also fill the volume until it is full (slow: free space / write speed)
#   --bench         also run Bench/run.sh on it (results in Bench/results/)
#   --cut           also cut writes off: kill the daemon, and (with you) pull the disk out. Run
#                   last; they leave the volume DIRTY and often damaged, as on Windows: when the
#                   check after them finds damage, the disk needs Windows chkdsk (or reformatting)
#                   before it can be written again
#   --only STEPS    run only these steps, comma-separated (for example --only replug,pull,sleep)
#
# Steps, each PASS / FAIL / SKIP:
#   doctor          `unlatch doctor` passes
#   rw-suite        Tests/Fuse/rw-suite.sh --device on each backend this Mac can use
#   big-file        a 4.5 GiB file (over FAT32's limit), read back after a remount
#   small-files     thousands of small files, checked after a remount, deleted, space back
#   snapshot        the volume copied through the helper; ntfsfix -n and the engine read it,
#                   and every file through a read-only mount matches the engine (check-readonly)
#   fill            write until ENOSPC; the volume stays clean; the space comes back
#   replug          eject, unplug, plug back in: auto-mount (or a CLI mount) sees the files
#   sleep           sleep and wake the Mac with the disk mounted: the mount survives
#   bench           speeds, against the earlier results
#   kill (--cut)    the daemon SIGKILLed mid-copy: DIRTY, read-only (D-86), earlier file intact,
#                   then the snapshot check; marked clean again only if that finds nothing
#   pull (--cut)    pull the disk out mid-copy: the daemon exits within 5 s (D-85), no hang,
#                   then the same as kill
#
# Only the folders unlatch-test/ and rw-suite/ on the volume are written; other files stay as
# they are. Still, use a disk whose content you can lose: some steps cut writes off on purpose.
# Needs a development build (AGENTS.md: xcodebuild -scheme Unlatch, swift build --package-path
# CLI), the privileged helper, and macFUSE. Report: build/real-disk/<time>-<label>.txt.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
products="$root/build/DerivedData/Build/Products/Debug"
fuse="$products/unlatch-fuse" engine="$products/unlatch-engine" ntfsfix="$root/build/ntfs3g/bin/ntfsfix"
cli="${UNLATCH_CLI:-$root/CLI/.build/debug/unlatch}" withdev="$root/Tests/Fuse/with-device.py"
src="$root/Engine/vendor/ntfs-3g"

disk="" yes=0 quick=0 physical=1 fill=0 bench=0 cut=0 only=""
while [ $# -gt 0 ]; do
    case "$1" in
        --yes) yes=1 ;; --quick) quick=1 ;; --no-physical) physical=0 ;; --fill) fill=1 ;; --bench) bench=1 ;;
        --only) only=",$2,"; shift ;;
        --cut) cut=1 ;;
        -h|--help) sed -n '3,41p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "test-real-disk: unknown option $1" >&2; exit 64 ;;
        *) [ -z "$disk" ] && disk="$1" || { echo "test-real-disk: one disk only" >&2; exit 64; } ;;
    esac
    shift
done
[ -n "$disk" ] || { sed -n '5p' "$0" | sed 's/^# *//' >&2; exit 64; }

for f in "$fuse" "$engine" "$cli" "$ntfsfix"; do
    [ -x "$f" ] || { echo "test-real-disk: $f is missing; build first (AGENTS.md, Build and test)" >&2; exit 1; }
done

# ---- the volume ----------------------------------------------------------------------------
# list --json -> "bsd|label|size|mountpoint|internal" (not tabs: read would merge empty fields) for the given name, label or path
find_vol() {
    "$cli" list --json 2>/dev/null | python3 -I -c '
import json, sys
want = sys.argv[1].removeprefix("/dev/")
for v in json.load(sys.stdin)["volumes"]:
    if want in (v["bsdName"], v["label"], v.get("mountPoint")) or (sys.argv[2] and v["label"] == sys.argv[2] and str(v["size"]) == sys.argv[3]):
        print("|".join([v["bsdName"], v["label"], str(v["size"]), v.get("mountPoint") or "", str(v["internal"]).lower()]))
        break' "$1" "${2:-}" "${3:-}"
}
IFS='|' read -r bsd label size _ internal <<< "$(find_vol "$disk")"
[ -n "${bsd:-}" ] || { echo "test-real-disk: no NTFS volume '$disk' (see: unlatch list)" >&2; exit 1; }
[ "$internal" = false ] || { echo "test-real-disk: $bsd is an internal disk; refusing" >&2; exit 1; }

work="$root/build/real-disk"; mkdir -p "$work"
stamp=$(date +%Y%m%d-%H%M%S)
report="$work/$stamp-$label.txt" log="$work/$stamp-$label.log"
: > "$report"; : > "$log"
fails=0
say() { printf '%s\n' "$*" | tee -a "$report"; }
result() {     # step, PASS|FAIL|SKIP, detail
    printf '%-12s %-4s  %s\n' "$1" "$2" "$3" | tee -a "$report"
    [ "$2" = FAIL ] && fails=$((fails + 1))
    return 0
}
logrun() { echo "\$ $*" >> "$log"; "$@" >> "$log" 2>&1; }

cur_mount() { IFS='|' read -r _ _ _ m _ <<< "$(find_vol "$bsd")"; printf '%s' "${m:-}"; }
mount_vol() {  # [-r]; prints the mount point; returns the CLI's exit (3: mounted, forced read-only)
    local out rc=0; out=$("$cli" mount --json "$@" "$bsd" 2>> "$log") || rc=$?
    [ $rc = 0 ] || [ $rc = 3 ] || return $rc
    printf '%s' "$out" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["mountPoint"])' || return 1
    return $rc
}
unmount_vol() { for _ in 1 2 3 4 5 6 7 8 9 10; do "$cli" unmount "$bsd" >> "$log" 2>&1 && return 0; sleep 1; done; return 1; }
probe_state() { "$withdev" "$bsd" ro "$engine" probe {} 2>> "$log" | awk '/^state:/ {print $2}'; }
daemon_pid() { pgrep -f "unlatch-fuse -f .*/dev/$bsd " | head -1; }
gone() { for _ in $(seq 1 60); do mount | grep -q " on $1 (" || return 0; sleep 0.5; done; return 1; }
ask() {        # prompt; waits for Enter
    printf '\n>>> %s\n    Press Enter when done (or type s and Enter to skip): ' "$1" > /dev/tty
    local a; read -r a < /dev/tty; [ "$a" != s ]
}
wait_disk() {  # gone|back, seconds: waits for the volume to disappear or come back (any BSD name)
    for _ in $(seq 1 "$2"); do
        local found; found=$(find_vol "" "$label" "$size")
        if [ "$1" = gone ] && [ -z "$found" ]; then return 0; fi
        if [ "$1" = back ] && [ -n "$found" ]; then bsd=$(cut -d'|' -f1 <<< "$found"); return 0; fi
        sleep 1
    done
    return 1
}
hashes() { (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256); }

# ---- confirm -------------------------------------------------------------------------------
say "test-real-disk $stamp: $bsd \"$label\", $((size / 1000000)) MB, $(git -C "$root" describe --tags --always --dirty 2>/dev/null)"
say "  $(diskutil info "${bsd%s[0-9]*}" 2>/dev/null | awk -F': *' '/Device \/ Media Name/ {print $2}'), $(sw_vers -productVersion), $("$cli" doctor 2>/dev/null | awk '/macFUSE installed/ {print $NF}' | tr -d '()') macFUSE"
m=$(cur_mount); [ -n "$m" ] || m=$(mount_vol -r) || { echo "test-real-disk: cannot mount $bsd to look at it" >&2; exit 1; }
others=$(ls -A "$m" 2>/dev/null | grep -v -x -E 'unlatch-test|rw-suite|\.DS_Store|\.fseventsd|\.Spotlight-V100|\.Trashes|\.TemporaryItems|System Volume Information|\$RECYCLE\.BIN' || true)
if [ -n "$others" ]; then
    say "  the volume also holds: $(printf '%s' "$others" | head -5 | tr '\n' ' ')$( [ "$(printf '%s\n' "$others" | wc -l)" -gt 5 ] && echo ...)"
fi
if [ $yes = 0 ]; then
    printf '\nThis writes to "%s" (%s) and cuts writes off on purpose. Use a disk you can lose.\nType the label to go on: ' "$label" "$bsd" > /dev/tty
    read -r typed < /dev/tty
    [ "$typed" = "$label" ] || { echo "test-real-disk: not confirmed" >&2; exit 1; }
fi
unmount_vol || true

# The app claims auto-mounted disks from Apple's driver (DiskWatcher(claim:)); without it,
# macOS mounts the disk read-only after an unmount, sometimes beside Unlatch's own mount.
if ! pgrep -qf "Unlatch.app/Contents/MacOS/Unlatch"; then
    say "  note: the Unlatch app is not running: replug mounts with the CLI, and Apple's driver may mount the disk too"
fi

backends="fskit"
"$cli" doctor 2>/dev/null | grep -q "kernel extension (allowed" && backends="kext fskit"

# ---- steps ---------------------------------------------------------------------------------
step_doctor() {
    if logrun "$cli" doctor; then result doctor PASS "every check ok"; else result doctor FAIL "see the log"; fi
}

step_rwsuite() {
    local b
    for b in $backends; do
        echo "\$ rw-suite --device $bsd --backend $b" >> "$log"
        if UNLATCH_CLI="$cli" "$root/Tests/Fuse/rw-suite.sh" --runs 1 --device "$bsd" --backend "$b" >> "$log" 2>&1; then
            result rw-suite PASS "$b backend: copies, renames, deletes, xattrs, links, names, git, remount"
        else
            result rw-suite FAIL "$b backend: $(grep FAIL "$log" | tail -1)"
            unmount_vol || true
        fi
    done
}

step_bigfile() {
    local mib=$((quick ? 1100 : 4608)) f="$work/big-$((quick ? 1100 : 4608)).bin" m t0 secs
    [ -f "$f" ] && [ "$(stat -f %z "$f")" = $((mib << 20)) ] || head -c $((mib << 20)) /dev/urandom > "$f"
    local want; want=$(shasum -a 256 < "$f" | cut -d' ' -f1)
    m=$(mount_vol) || { result big-file FAIL "mount"; return; }
    mkdir -p "$m/unlatch-test"; rm -f "$m/unlatch-test/big.bin"
    t0=$(date +%s)
    cp "$f" "$m/unlatch-test/big.bin" 2>> "$log" || { result big-file FAIL "copy: $(tail -1 "$log")"; unmount_vol; return; }
    secs=$(( $(date +%s) - t0 ))
    unmount_vol || { result big-file FAIL "unmount"; return; }
    m=$(mount_vol -r) || { result big-file FAIL "read-only remount"; return; }
    local got; got=$(shasum -a 256 < "$m/unlatch-test/big.bin" | cut -d' ' -f1)
    unmount_vol
    m=$(mount_vol) && rm -f "$m/unlatch-test/big.bin"; unmount_vol
    if [ "$got" = "$want" ]; then
        result big-file PASS "$mib MiB written in ${secs}s ($((mib / (secs > 0 ? secs : 1))) MiB/s), same after a remount"
    else
        result big-file FAIL "$mib MiB file differs after a remount"
    fi
}

step_smallfiles() {
    local n=$((quick ? 2000 : 10000)) m before after
    m=$(mount_vol) || { result small-files FAIL "mount"; return; }
    rm -rf "${m:?}/unlatch-test/small"; mkdir -p "$m/unlatch-test/small"
    before=$(df -k "$m" | awk 'NR==2 {print $4}')
    python3 -I - "$m/unlatch-test/small" "$n" <<'PY' 2>> "$log"
import os, random, sys
d, n = sys.argv[1], int(sys.argv[2]); r = random.Random(7)
for i in range(n):
    sub = os.path.join(d, f"d{i % 100:02d}"); os.makedirs(sub, exist_ok=True)
    with open(os.path.join(sub, f"f{i:05d}.bin"), "wb") as f: f.write(r.randbytes(r.randint(1, 8192)))
PY
    hashes "$m/unlatch-test/small" > "$work/small.sha"
    unmount_vol || { result small-files FAIL "unmount"; return; }
    m=$(mount_vol) || { result small-files FAIL "remount"; return; }
    if ! diff "$work/small.sha" <(hashes "$m/unlatch-test/small") > /dev/null; then
        result small-files FAIL "$n files differ after a remount"; unmount_vol; return
    fi
    rm -r "$m/unlatch-test/small"; after=$(df -k "$m" | awk 'NR==2 {print $4}')
    unmount_vol
    # Space back within 2 MiB (directory indexes may keep a few clusters), plus 1 KiB per file and
    # folder created: NTFS grows $MFT by one record each and never shrinks it. On a freshly
    # formatted stick that is ~10 MiB the first time (2026-11-03: 1,496 -> 11,166 records).
    if [ $((before - after)) -le $((2048 + n + 100)) ]; then result small-files PASS "$n files checked after a remount, deleted, space back"
    else result small-files FAIL "$(( (before - after) / 1024 )) MiB not freed after rm -r"; fi
}

step_kill() {
    local m pid d state n
    m=$(mount_vol) || { result kill FAIL "mount"; return; }
    rm -rf "${m:?}/unlatch-test/copy"; mkdir -p "$m/unlatch-test/copy"
    echo before > "$m/unlatch-test/before-kill.txt"; sync; sleep 2      # written back (D-62)
    (cd "$m/unlatch-test" && shasum -a 256 before-kill.txt) > "$work/before-kill.sha"
    ( for i in 1 2 3 4 5 6; do cp -R "$src" "$m/unlatch-test/copy/$i" 2>/dev/null || exit 0; done ) &
    pid=$!
    sleep 4
    d=$(daemon_pid)
    [ -n "$d" ] || { result kill FAIL "no daemon found"; kill $pid; wait $pid 2>/dev/null; unmount_vol; return; }
    kill -9 "$d"; wait $pid 2>/dev/null
    gone "$m" || { umount -f "$m" 2>/dev/null; gone "$m"; }
    "$cli" list > /dev/null 2>&1          # reaps a dead daemon's mount
    after_cut kill "SIGKILL"
}

# After a cut-off write (kill or pull): the volume is DIRTY (D-86), mounts read-only (CLI
# exit 3), and a file written back before the cut is intact. Then the snapshot check, and
# only if that passes the volume is marked clean again, so the later steps can write.
after_cut() {     # step, what; the file is unlatch-test/before-<step>.*
    local state m rc n
    state=$(probe_state)
    [ "$state" = dirty ] || { result "$1" FAIL "probe after the $2: ${state:-nothing} (want dirty, D-86)"; return; }
    m=$(mount_vol); rc=$?
    [ $rc = 3 ] || { result "$1" FAIL "mount after the $2: exit $rc (want 3, read-only)"; [ $rc = 0 ] && unmount_vol; return; }
    n=$(find "$m/unlatch-test/copy" -type f 2>/dev/null | wc -l | tr -d ' ')
    if ! (cd "$m/unlatch-test" && shasum -a 256 -c --status "$work/before-$1.sha") 2>/dev/null; then
        result "$1" FAIL "the file written before the $2 differs or is gone"; unmount_vol; return
    fi
    unmount_vol
    result "$1" PASS "probe dirty, mounted read-only, $n files of the copy present, earlier file intact"
    if step_snapshot "$1-check" dirty; then mark_clean "$1"
    else say "  $label stays DIRTY and read-only: the $2 left damage, as on Windows. Run chkdsk on Windows (or reformat it) before writing to it again."; fi
}

# The volume was checked (snapshot, ntfsfix -n, check-readonly): clear its dirty mark.
mark_clean() {
    if "$withdev" "$bsd" rw "$engine" mark-clean {} >> "$log" 2>&1 && [ "$(probe_state)" = clean ]; then
        result "$1-clean" PASS "checked, marked clean again"
    else
        result "$1-clean" FAIL "could not mark the checked volume clean; later steps run read-only"
    fi
}

step_snapshot() {     # name [state, default clean]; returns 0 on PASS
    local img="$work/snapshot.img" m want="${2:-clean}"
    unmount_vol 2>/dev/null || true
    rm -f "$img"
    if ! "$withdev" "$bsd" ro --snapshot "$img" >> "$log" 2>&1; then result "$1" FAIL "snapshot through the helper"; return 1; fi
    if ! logrun "$ntfsfix" -n "$img"; then result "$1" FAIL "ntfsfix -n: $(tail -1 "$log")"; rm -f "$img"; return 1; fi
    "$engine" probe "$img" | grep -q "^state: *$want" || { result "$1" FAIL "engine probe of the snapshot: $("$engine" probe "$img" | head -1) (want $want)"; rm -f "$img"; return 1; }
    local rc=0
    m=$(mount_vol -r); rc=$?           # 3: a DIRTY volume reports "forced read-only" even with -r
    [ $rc = 0 ] || [ $rc = 3 ] || { result "$1" FAIL "read-only mount (exit $rc)"; rm -f "$img"; return 1; }
    rc=0
    if UNLATCH_ENGINE="$engine" logrun "$root/Tests/Fuse/check-readonly.sh" "$img" "$m"; then
        result "$1" PASS "ntfsfix -n ok, $want, $(tail -1 "$log" | sed 's/^check-readonly: //')"
    else
        result "$1" FAIL "mount and engine disagree: $(grep FAIL "$log" | tail -1)"; rc=1
    fi
    unmount_vol; rm -f "$img"
    return $rc
}

step_fill() {
    local m before after
    m=$(mount_vol) || { result fill FAIL "mount"; return; }
    mkdir -p "$m/unlatch-test"; before=$(df -k "$m" | awk 'NR==2 {print $4}')
    say "  fill: writing $((before / 1024)) MiB, this takes a while"
    dd if=/dev/zero of="$m/unlatch-test/fill.bin" bs=4m 2> "$work/fill.txt"
    grep -q -i "no space" "$work/fill.txt" || { result fill FAIL "dd did not end with ENOSPC: $(tail -1 "$work/fill.txt")"; rm -f "$m/unlatch-test/fill.bin"; unmount_vol; return; }
    echo small > "$m/unlatch-test/after-full.txt" 2>/dev/null; rm -f "$m/unlatch-test/after-full.txt"
    unmount_vol || { result fill FAIL "unmount when full"; return; }
    [ "$(probe_state)" = clean ] || { result fill FAIL "not clean when full"; return; }
    m=$(mount_vol) || { result fill FAIL "remount when full"; return; }
    rm -f "$m/unlatch-test/fill.bin"; after=$(df -k "$m" | awk 'NR==2 {print $4}')
    unmount_vol
    if [ $((before - after)) -le 4096 ]; then result fill PASS "filled to ENOSPC, stayed clean, $((after / 1024)) MiB free again"
    else result fill FAIL "$(( (before - after) / 1024 )) MiB not free again"; fi
}

step_replug() {
    local m want
    m=$(mount_vol) || { result replug FAIL "mount"; return; }
    mkdir -p "$m/unlatch-test"; head -c 5000000 /dev/urandom > "$m/unlatch-test/replug.bin"
    want=$(shasum -a 256 < "$m/unlatch-test/replug.bin" | cut -d' ' -f1)
    logrun "$cli" eject "$bsd" || { result replug FAIL "eject"; return; }
    ask "Unplug \"$label\", wait 5 seconds and plug it back in." || { result replug SKIP "skipped"; wait_disk back 60; return; }
    wait_disk back 60 || { result replug FAIL "the disk did not come back within 60 s"; return; }
    local auto="no"
    for _ in $(seq 1 30); do m=$(cur_mount); [ -n "$m" ] && { auto=yes; break; }; sleep 1; done
    [ -n "$m" ] || m=$(mount_vol) || { result replug FAIL "mount after replug"; return; }
    local got; got=$(shasum -a 256 < "$m/unlatch-test/replug.bin" | cut -d' ' -f1)
    rm -f "$m/unlatch-test/replug.bin"; unmount_vol
    if [ "$got" = "$want" ]; then result replug PASS "back as $bsd, $( [ $auto = yes ] && echo "auto-mounted by the app" || echo "mounted with the CLI (no auto-mount)"), file intact"
    else result replug FAIL "file differs after a replug"; fi
}

step_pull() {
    local m pid d t0
    m=$(mount_vol) || { result pull FAIL "mount"; return; }
    rm -rf "${m:?}/unlatch-test/copy"; mkdir -p "$m/unlatch-test/copy"
    head -c 5000000 /dev/urandom > "$m/unlatch-test/before-pull.bin"
    (cd "$m/unlatch-test" && shasum -a 256 before-pull.bin) > "$work/before-pull.sha"
    sync; sleep 2                        # written back (D-62: within a second)
    d=$(daemon_pid)
    ( for i in $(seq 1 30); do cp -R "$src" "$m/unlatch-test/copy/$i" 2>/dev/null || exit 0; done ) &
    pid=$!
    if ! ask "Files are being copied to \"$label\". Pull it out NOW, without ejecting. Leave it out."; then
        kill $pid 2>/dev/null; wait $pid 2>/dev/null; unmount_vol; result pull SKIP "skipped"; return
    fi
    t0=$(date +%s)
    wait_disk gone 30 || { result pull FAIL "the disk is still attached"; kill $pid; wait $pid 2>/dev/null; return; }
    # The daemon notices within a second or two and unmounts itself (D-85).
    local waited=0
    while kill -0 "$d" 2>/dev/null && [ $waited -lt 30 ]; do sleep 1; waited=$((waited + 1)); done
    if kill -0 "$d" 2>/dev/null; then result pull FAIL "the daemon still runs 30 s after the pull"; kill -9 "$d"; return
    elif [ $waited -gt 5 ]; then result pull FAIL "the daemon took ${waited}s to notice the pull (more than 5 s)"; fi
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
    gone "$m" || { umount -f "$m" 2>/dev/null; gone "$m" || { result pull FAIL "stale mount at $m"; return; }; }
    ls /Volumes > /dev/null 2>&1 & local lsp=$!          # macOS has no timeout(1)
    for _ in $(seq 1 10); do kill -0 $lsp 2>/dev/null || break; sleep 1; done
    if kill -0 $lsp 2>/dev/null; then kill -9 $lsp; result pull FAIL "/Volumes hangs after the pull"; return; fi
    ask "Plug \"$label\" back in." || { result pull SKIP "not plugged back"; return; }
    wait_disk back 60 || { result pull FAIL "the disk did not come back"; return; }
    sleep 3; m=$(cur_mount); [ -z "$m" ] || unmount_vol      # the app may have mounted it (read-only)
    result pull PASS "the daemon exited ${waited}s after the disk went away; no hang"
    after_cut pull "pull"
}

step_sleep() {
    local m want since d
    m=$(mount_vol) || { result sleep FAIL "mount"; return; }
    mkdir -p "$m/unlatch-test"; head -c 5000000 /dev/urandom > "$m/unlatch-test/sleep.bin"
    want=$(shasum -a 256 < "$m/unlatch-test/sleep.bin" | cut -d' ' -f1); d=$(daemon_pid)
    since=$(date '+%Y-%m-%d %H:%M:%S')
    ask "Put the Mac to sleep (Apple menu > Sleep), wait 30 seconds, then wake it." || { unmount_vol; result sleep SKIP "skipped"; return; }
    local slept; slept=$(pmset -g log 2>/dev/null | awk -v s="$since" '$1" "$2 >= s && /Entering Sleep/' | wc -l | tr -d ' ')
    m=$(cur_mount)
    [ -n "$m" ] && kill -0 "$d" 2>/dev/null || { result sleep FAIL "the mount did not survive (slept: $slept)"; return; }
    local got; got=$(shasum -a 256 < "$m/unlatch-test/sleep.bin" | cut -d' ' -f1)
    echo after > "$m/unlatch-test/after-sleep.txt" || { result sleep FAIL "write after wake"; unmount_vol; return; }
    rm -f "$m/unlatch-test/sleep.bin" "$m/unlatch-test/after-sleep.txt"
    unmount_vol; [ "$(probe_state)" = clean ] || { result sleep FAIL "not clean after unmount"; return; }
    [ "$got" = "$want" ] || { result sleep FAIL "file differs after wake"; return; }
    if [ "$slept" -gt 0 ]; then result sleep PASS "slept and woke; mount, daemon and file intact; writes work; clean"
    else result sleep PASS "mount intact (pmset shows no sleep since $since: did the Mac sleep?)"; fi
}

step_bench() {
    local m b
    for b in $backends; do
        m=$(UNLATCH_BACKEND=$b mount_vol) || { result bench FAIL "$b mount"; continue; }
        if logrun "$root/Bench/run.sh" "$m" "unlatch-$b-real-$label" --runs 3 --mib 256 --small 3000 --meta 2000; then
            result bench PASS "$b: $(grep -E 'seq_write|seq_read_cold|small_files|metadata' "$log" | tail -4 | tr -s ' ' | tr '\n' ';')"
        else result bench FAIL "$b: see the log"; fi
        unmount_vol
    done
}

cleanup() {     # leaves the volume unmounted: it is DIRTY while mounted read-write (D-86)
    local m; m=$(cur_mount); [ -n "$m" ] || m=$(mount_vol 2>/dev/null) || return
    rm -rf "${m:?}/unlatch-test" "${m:?}/rw-suite"
    unmount_vol
}

say "backends: $backends$( [ $quick = 1 ] && echo ', quick')$( [ -n "$only" ] && echo ", only${only%,}")"
want() { [ -z "$only" ] || [[ "$only" == *",$1,"* ]]; }
want doctor && step_doctor
want rw-suite && step_rwsuite
want big-file && step_bigfile
want small-files && step_smallfiles
want snapshot && step_snapshot snapshot
if want fill; then if [ $fill = 1 ] || [ -n "$only" ]; then step_fill; else result fill SKIP "not asked (--fill)"; fi; fi
for s in replug sleep; do
    want $s || continue
    if [ $physical = 1 ] && { : < /dev/tty; } 2>/dev/null; then "step_$s"
    else why="--no-physical"; [ $physical = 1 ] && why="no terminal to ask you in"; result $s SKIP "$why"; fi
done
if want bench; then if [ $bench = 1 ] || [ -n "$only" ]; then step_bench; else result bench SKIP "not asked (--bench)"; fi; fi
# Last: they cut writes off and can leave the disk needing chkdsk.
if want kill; then if [ $cut = 1 ] || [ -n "$only" ]; then step_kill; else result kill SKIP "not asked (--cut)"; fi; fi
if want pull; then
    if [ $cut = 0 ] && [ -z "$only" ]; then result pull SKIP "not asked (--cut)"
    elif [ $physical = 1 ] && { : < /dev/tty; } 2>/dev/null; then step_pull
    else result pull SKIP "$( [ $physical = 1 ] && echo "no terminal to ask you in" || echo --no-physical)"; fi
fi
cleanup
case "$(probe_state)" in
    clean) result final PASS "volume clean, test folders removed" ;;
    dirty) if grep -q -E '^(kill|pull)-check +FAIL' "$report"; then result final SKIP "DIRTY after a cut, as expected: needs chkdsk"
           else result final FAIL "volume DIRTY at the end"; fi ;;
    *) result final FAIL "volume not clean at the end" ;;
esac
"$cli" mount "$bsd" > /dev/null 2>&1 || true

say ""
say "$( [ $fails = 0 ] && echo "test-real-disk: PASSED" || echo "test-real-disk: $fails step(s) FAILED")  report: $report  log: $log"
[ $fails = 0 ]
