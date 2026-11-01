#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks the toolchain, then generates Unlatch.xcodeproj from project.yml.
set -euo pipefail

cd "$(dirname "$0")/.."

fail() { echo "bootstrap: $*" >&2; exit 1; }

xcodebuild -version >/dev/null 2>&1 || fail "Xcode not found. Install Xcode 27+ and run: sudo xcode-select -s /Applications/Xcode.app"
xcode_major=$(xcodebuild -version | awk 'NR==1 { split($2, v, "."); print v[1] }')
[ "$xcode_major" -ge 27 ] || fail "Xcode 27+ required, found $(xcodebuild -version | head -1)"

command -v xcodegen >/dev/null || fail "XcodeGen not found. Install with: brew install xcodegen"
command -v git-lfs >/dev/null || fail "Git LFS not found. Install with: brew install git-lfs"

git lfs install --local >/dev/null
xcodegen generate

echo "bootstrap: OK ($(xcodebuild -version | head -1), XcodeGen $(xcodegen --version | awk '{print $2}'), $(git lfs version | awk '{print $1}'))"
