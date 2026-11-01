#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks every golden image in Tests/Images against its <name>.expect file:
#   state S | label L | count DIR N | file PATH SIZE SHA256 | flag PATH FLAG | init PATH N
#   absent PATH | rw_open 0|EPERM|EOPNOTSUPP|EROFS | stream PATH NAME SIZE SHA256 | link PATH TARGET
# Fields are separated by spaces, or by tabs when the line has one (names with spaces).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
eng="$root/build/DerivedData/Build/Products/Debug/unlatch-engine"
out="$root/build/golden"
[ -x "$eng" ] || xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build > /dev/null
rm -rf "$out"
mkdir -p "$out"

fails=0 checks=0
shopt -s nullglob
images=(Tests/Images/*.img.zst)
[ ${#images[@]} -gt 0 ] || { echo "test-golden: no images (git lfs pull?)" >&2; exit 1; }
for z in "${images[@]}"; do
    name="$(basename "$z" .img.zst)"
    img="$out/$name.img"
    exp="Tests/Images/$name.expect"
    if head -c 100 "$z" | grep -q "git-lfs.github.com"; then
        echo "FAIL $name: LFS pointer, not the image (run git lfs pull)" >&2; fails=$((fails + 1)); continue
    fi
    zstd -q -d --sparse -f "$z" -o "$img"
    [ -f "$exp" ] || { echo "FAIL $name: no $exp" >&2; fails=$((fails + 1)); continue; }
    probe="$("$eng" probe "$img")"
    bad=0
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        if [[ "$line" == *$'\t'* ]]; then IFS=$'\t' read -r kind a b c d <<< "$line"
        else read -r kind a b c d <<< "$line"; fi
        checks=$((checks + 1))
        case "$kind" in
            state) grep -q "^state: *$a$" <<< "$probe" || { echo "  $name: state is not $a" >&2; bad=1; } ;;
            label) grep -q "^label: *$a$" <<< "$probe" || { echo "  $name: label is not $a" >&2; bad=1; } ;;
            count) n="$("$eng" ls "$img" "$a" | wc -l | tr -d ' ')"
                   [ "$n" = "$b" ] || { echo "  $name: $a has $n entries, want $b" >&2; bad=1; } ;;
            file) s="$("$eng" stat "$img" "$a" | awk '/^size:/ { print $2 }')"
                  h="$("$eng" cat "$img" "$a" | shasum -a 256 | cut -d' ' -f1)"
                  [ "$s" = "$b" ] && [ "$h" = "$c" ] || { echo "  $name: $a size $s sha $h" >&2; bad=1; } ;;
            flag) "$eng" stat "$img" "$a" | grep -q "^flags:.* $b" || { echo "  $name: $a lacks flag $b" >&2; bad=1; } ;;
            init) "$eng" stat "$img" "$a" | grep -q "^init_size: *$b$" || { echo "  $name: $a init_size is not $b" >&2; bad=1; } ;;
            stream) s="$("$eng" xattr "$img" "$a" "$b" | wc -c | tr -d ' ')"
                    h="$("$eng" xattr "$img" "$a" "$b" | shasum -a 256 | cut -d' ' -f1)"
                    [ "$s" = "$c" ] && [ "$h" = "$d" ] || { echo "  $name: $a:$b size $s sha $h" >&2; bad=1; } ;;
            link) st="$("$eng" stat "$img" "$a")"
                  grep -q "^type: *symlink$" <<< "$st" && [ "$(sed -n 's/^target: *//p' <<< "$st")" = "$b" ] \
                      || { echo "  $name: $a is not a link to $b ($(sed -n 's/^target: *//p' <<< "$st"))" >&2; bad=1; } ;;
            absent) if "$eng" stat "$img" "$a" > /dev/null 2>&1; then echo "  $name: $a exists" >&2; bad=1; fi ;;
            rw_open) cp -c "$img" "$out/rw.img"
                     msg="$("$eng" info --rw "$out/rw.img" 2>&1 || true)"
                     case "$a" in
                         0) grep -q "^mode: *read-write$" <<< "$msg" ;;
                         EPERM) grep -q "Operation not permitted" <<< "$msg" ;;
                         EOPNOTSUPP) grep -q "Operation not supported" <<< "$msg" ;;
                         EROFS) grep -q "Read-only file system" <<< "$msg" ;;
                     esac || { echo "  $name: read-write open gave: $msg" >&2; bad=1; } ;;
            *) echo "  $name: unknown expectation '$kind'" >&2; bad=1 ;;
        esac
    done < "$exp"
    if [ "$bad" = 0 ]; then echo "ok   $name"; else echo "FAIL $name" >&2; fails=$((fails + 1)); fi
done
echo "test-golden: ${#images[@]} images, $checks checks, $fails failed"
[ "$fails" = 0 ]
