#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the vendored libntfs-3g (static, arm64) and ntfsprogs with no FUSE driver.
# Output: build/ntfs3g/{lib,include,bin}. Source tree stays untouched (out-of-tree build).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/Engine/vendor/ntfs-3g"
patches="$root/Engine/vendor/patches"
work="$root/build/ntfs3g-src"
obj="$root/build/ntfs3g-obj"
prefix="$root/build/ntfs3g"
jobs="$(sysctl -n hw.ncpu)"

[ -d "$src" ] || { echo "build-ntfs3g: $src missing" >&2; exit 1; }

# Copy the source so patches and autoreconf never modify the vendored tree.
rm -rf "$work" "$obj" "$prefix"
mkdir -p "$work" "$obj"
cp -R "$src/." "$work/"

shopt -s nullglob
for p in "$patches"/*.patch; do
    echo "build-ntfs3g: applying $(basename "$p")"
    patch -d "$work" -p1 --forward --quiet < "$p"
done
shopt -u nullglob

if [ ! -x "$work/configure" ]; then
    command -v autoreconf >/dev/null || { echo "build-ntfs3g: autoreconf needed (brew install autoconf automake libtool)" >&2; exit 1; }
    (cd "$work" && autoreconf -fi)
fi

export CC="$(xcrun -f clang)"
export CFLAGS="-arch arm64 -mmacosx-version-min=27.0 -O2 -g"
export LDFLAGS="-arch arm64 -mmacosx-version-min=27.0"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"

# --exec-prefix stops configure from installing into /bin, /sbin and /lib.
cd "$obj"
"$work/configure" \
    --prefix="$prefix" \
    --exec-prefix="$prefix" \
    --host=aarch64-apple-darwin \
    --disable-ntfs-3g \
    --disable-mount-helper \
    --enable-ntfsprogs \
    --disable-crypto \
    --disable-plugins \
    --disable-nfconv \
    --disable-shared \
    --enable-static \
    --disable-ldconfig \
    --disable-mtab \
    --disable-dependency-tracking \
    --enable-silent-rules \
    > configure.log 2>&1 || { tail -40 configure.log >&2; exit 1; }

make -j"$jobs" > make.log 2>&1 || { grep -E 'error' make.log | head -40 >&2; tail -20 make.log >&2; exit 1; }
make install > install.log 2>&1 || { tail -40 install.log >&2; exit 1; }

# sbin tools (mkntfs, ntfsfix, ...) go next to the bin tools.
if [ -d "$prefix/sbin" ]; then
    mkdir -p "$prefix/bin"
    mv "$prefix/sbin/"* "$prefix/bin/"
    rmdir "$prefix/sbin"
fi

echo "build-ntfs3g: $(lipo -info "$prefix/lib/libntfs-3g.a")"
echo "build-ntfs3g: tools: $(ls "$prefix/bin" | tr '\n' ' ')"
