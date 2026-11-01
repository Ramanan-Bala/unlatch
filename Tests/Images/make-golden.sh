#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerates the golden images made WITHOUT Windows (see README.md for which ones and why).
# Output: Tests/Images/<name>.img.zst (Git LFS). Expectations live in <name>.expect and are
# written by hand from how each image is built, not from the engine's output.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
work="$root/build/golden-make"
bin="$root/build/ntfs3g/bin"
rm -rf "$work"
mkdir -p "$work"
[ -f build/ntfs3g/lib/libntfs-3g.a ] || Scripts/build-ntfs3g.sh
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -DHAVE_CONFIG_H=1 -Ibuild/ntfs3g/config -isystem build/ntfs3g/include \
    -IEngine/src/fixtures -o "$work/mkfixture" Engine/src/fixtures/mkfixture.c build/ntfs3g/lib/libntfs-3g.a
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -o "$work/set_dirty" Tests/Engine/set_dirty.c
cd "$work"

long="$(printf 'L%.0s' $(seq 1 255))"                        # a 255-character name
seg() { printf "$1%.0s" $(seq 1 60); }                        # a 60-character component
deep_dirs="mkdir /deep"
deep="/deep"
for c in a b c d e; do
    deep="$deep/$(seg $c)"
    deep_dirs="$deep_dirs
mkdir $deep"
done                                                          # full path: 300+ characters

content() {   # $1 = image
    "$work/mkfixture" "$1" <<FIXTURE
mkdir /docs
mkdir /docs/sub
file /docs/sub/report.bin 200000 101
file /ReadMe.TXT 4321 102
file /café.txt 33 103
file /日本語.txt 77 104
file /small.txt 120 105
flags /small.txt hidden
sparse /sparse.bin 268435456 134217728 65536 106
uninit /uninit.bin 1048576 8192 107
stream /ReadMe.TXT Zone.Identifier 26 108
$deep_dirs
file $deep/$long 1000 109
mkdir /many
many /many 300
FIXTURE
}

mk() { "$root/Scripts/make-image.sh" "$1" "$2" "$3" > /dev/null; }

mk fresh-ntfs3g 16m FRESH
mk content-ntfs3g 512m CONTENT && content content-ntfs3g.img
cp content-ntfs3g.img dirty-ntfs3g.img && "$work/set_dirty" dirty-ntfs3g.img
mk hiberfil-ntfs3g 16m HIBER
{ printf 'HIBR'; head -c 8188 /dev/zero; } > hiberfil.sys
"$bin/ntfscp" -q hiberfil-ntfs3g.img hiberfil.sys hiberfil.sys
for m in clean unclean faststartup; do
    mk "logfile-$m-synthetic" 16m LOG
    echo "logfile $m" | "$work/mkfixture" "logfile-$m-synthetic.img"
done

for i in *.img; do
    zstd -q -19 --sparse -f "$i" -o "$root/Tests/Images/$i.zst"
    printf '%-40s %s\n' "$i" "$(du -h "$root/Tests/Images/$i.zst" | cut -f1)"
done

# ---- expectations (from how the images are built; the engine's output is never copied) ----
g() { "$work/mkfixture" gen "$@"; }
sha() { shasum -a 256 | cut -d' ' -f1; }
zeros() { [ "$1" -gt 0 ] && head -c "$1" /dev/zero || true; }
exp="$root/Tests/Images"

content_expect() {   # $1 = state
    cat <<EXPECT
# Built by Tests/Images/make-golden.sh with libntfs-3g (mkfixture); not made by Windows.
state $1
label CONTENT
count / 9
count /docs/sub 1
count /many 300
file /docs/sub/report.bin 200000 $(g 200000 101 | sha)
file /ReadMe.TXT 4321 $(g 4321 102 | sha)
file /readme.txt 4321 $(g 4321 102 | sha)
file /café.txt 33 $(g 33 103 | sha)
file /日本語.txt 77 $(g 77 104 | sha)
file /small.txt 120 $(g 120 105 | sha)
flag /small.txt hidden
flag /small.txt resident
file /sparse.bin 268435456 $( { zeros 134217728; g 65536 106 134217728; zeros $((268435456 - 134217728 - 65536)); } | sha)
flag /sparse.bin sparse
file /uninit.bin 1048576 $( { g 8192 107; zeros $((1048576 - 8192)); } | sha)
init /uninit.bin 8192
file $deep/$long 1000 $(g 1000 109 | sha)
file /many/f00299 0 $(zeros 0 | sha)
absent /many/f00300
absent /\$MFT
absent /\$Extend
EXPECT
}
content_expect clean > "$exp/content-ntfs3g.expect"
content_expect dirty > "$exp/dirty-ntfs3g.expect"
cat > "$exp/fresh-ntfs3g.expect" <<EXPECT
# mkntfs -Q, 16 MB, no files. Not made by Windows.
state clean
label FRESH
count / 0
EXPECT
cat > "$exp/hiberfil-ntfs3g.expect" <<EXPECT
# hiberfil.sys whose header starts "HIBR" (as Windows writes on hibernate), copied in with ntfscp.
state hibernated
label HIBER
file /hiberfil.sys 8192 $( { printf HIBR; zeros 8188; } | sha)
rw_open EPERM
EXPECT
cat > "$exp/logfile-clean-synthetic.expect" <<EXPECT
# Synthetic \$LogFile restart pages v1.1 with RESTART_VOLUME_IS_CLEAN (mkfixture logfile clean).
state clean
label LOG
rw_open 0
EXPECT
cat > "$exp/logfile-unclean-synthetic.expect" <<EXPECT
# Synthetic \$LogFile restart pages v1.1 with an open client and no clean flag.
state unclean_log
label LOG
rw_open EOPNOTSUPP
EXPECT
cat > "$exp/logfile-faststartup-synthetic.expect" <<EXPECT
# Synthetic \$LogFile restart pages v2.0, the version Windows Fast Startup leaves.
state hibernated
label LOG
rw_open EPERM
EXPECT
echo "expectations written"
