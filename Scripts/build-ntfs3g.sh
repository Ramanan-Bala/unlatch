#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the vendored libntfs-3g (static, arm64) and ntfsprogs with no FUSE driver.
# Output: build/ntfs3g/{lib,include,bin}. Source tree stays untouched (out-of-tree build).
#
#   Scripts/build-ntfs3g.sh          product build
#   Scripts/build-ntfs3g.sh --fuzz   ASan + UBSan + fuzzer coverage build into build/ntfs3g-fuzz,
#                                    with Homebrew LLVM (Apple clang has no libFuzzer runtime)
#   Scripts/build-ntfs3g.sh --driver stock ntfs-3g and lowntfs-3g FUSE drivers against macFUSE's
#                                    libfuse 2 into build/ntfs3g-driver: a benchmark baseline only
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/Engine/vendor/ntfs-3g"
patches="$root/Engine/vendor/patches"
variant=""
[ "${1:-}" = --fuzz ] && variant="-fuzz"
[ "${1:-}" = --driver ] && variant="-driver"
work="$root/build/ntfs3g$variant-src"
obj="$root/build/ntfs3g$variant-obj"
prefix="$root/build/ntfs3g$variant"
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

export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
if [ "$variant" = -fuzz ]; then
    llvm=""
    for p in "${LLVM_PREFIX:-}" "$(brew --prefix llvm 2>/dev/null)" "$(brew --prefix llvm@20 2>/dev/null)"; do
        [ -n "$p" ] && [ -x "$p/bin/clang" ] && { llvm="$p/bin/clang"; break; }
    done
    [ -n "$llvm" ] || { echo "build-ntfs3g: --fuzz needs Homebrew LLVM (brew install llvm) or LLVM_PREFIX" >&2; exit 1; }
    export CC="$llvm"
    export CFLAGS="-arch arm64 -mmacosx-version-min=27.0 -isysroot $SDKROOT -O1 -g -fno-omit-frame-pointer"
    export LDFLAGS="-arch arm64 -mmacosx-version-min=27.0 -isysroot $SDKROOT"
    # Sanitizers are added at make time only: configure's test programs built with ASan can
    # hang at startup (seen on the CI runner), and configure needs none of them.
    nosan="-fno-sanitize=alignment,shift-base"
    make_flags=(CFLAGS="$CFLAGS -fsanitize=address,undefined,fuzzer-no-link $nosan -fno-sanitize-recover=undefined"
                LDFLAGS="$LDFLAGS -fsanitize=address,undefined $nosan")
else
    export CC="$(xcrun -f clang)"
    export CFLAGS="-arch arm64 -mmacosx-version-min=27.0 -O2 -g"
    export LDFLAGS="-arch arm64 -mmacosx-version-min=27.0"
    make_flags=()
fi

driver=(--disable-ntfs-3g)
if [ "$variant" = -driver ]; then
    driver=(--enable-ntfs-3g --with-fuse=external)
    export PKG_CONFIG_PATH="${MACFUSE_PREFIX:-/usr/local}/lib/pkgconfig"
fi

# --exec-prefix stops configure from installing into /bin, /sbin and /lib.
cd "$obj"
"$work/configure" \
    --prefix="$prefix" \
    --exec-prefix="$prefix" \
    --host=aarch64-apple-darwin \
    "${driver[@]}" \
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

make -j"$jobs" ${make_flags[@]+"${make_flags[@]}"} > make.log 2>&1 || { grep -E 'error' make.log | head -40 >&2; tail -20 make.log >&2; exit 1; }
make install ${make_flags[@]+"${make_flags[@]}"} > install.log 2>&1 || { tail -40 install.log >&2; exit 1; }

# sbin tools (mkntfs, ntfsfix, ...) go next to the bin tools.
if [ -d "$prefix/sbin" ]; then
    mkdir -p "$prefix/bin"
    mv "$prefix/sbin/"* "$prefix/bin/"
    rmdir "$prefix/sbin"
fi

# The engine compiles against the same config.h, so ntfs-3g struct layouts match (D-13).
mkdir -p "$prefix/config"
cp "$obj/config.h" "$prefix/config/config.h"

echo "build-ntfs3g: $(lipo -info "$prefix/lib/libntfs-3g.a")"
echo "build-ntfs3g: tools: $(ls "$prefix/bin" | tr '\n' ' ')"
