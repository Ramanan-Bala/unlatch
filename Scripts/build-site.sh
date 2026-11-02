#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: Scripts/build-site.sh [outdir]
# Builds the GitHub Pages site from site/ into outdir (default build/site): copies the pages,
# the brand assets from docs/brand/, and fills in @VERSION@ from VERSION. Only these files are
# published; nothing else under docs/ is. Preview: python3 -m http.server -d build/site 8000
set -euo pipefail
cd "$(dirname "$0")/.."

out="${1:-build/site}"
version="$(cat VERSION)"

rm -rf "$out"
mkdir -p "$out/brand"
cp site/*.css "$out/"
for f in site/*.html; do
    sed "s/@VERSION@/$version/g" "$f" > "$out/$(basename "$f")"
done
for f in wordmark.svg wordmark-dark.svg app-icon.svg social-preview.png; do
    cp "docs/brand/$f" "$out/brand/"
done
touch "$out/.nojekyll"

if grep -rq '@VERSION@' "$out"; then
    echo "build-site: @VERSION@ left in $out" >&2
    exit 1
fi
echo "$out"
