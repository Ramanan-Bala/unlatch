#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The version lives in VERSION and is copied into every place that carries it (plan Step 18):
# project.yml (MARKETING_VERSION), UnlatchCore (Unlatch.version), the engine (ue_version) and
# the daemon's status records (UNLATCH_VERSION).
#
#   Scripts/set-version.sh X.Y.Z[-pre]     write VERSION and every copy, bump the build number
#   Scripts/set-version.sh --check         exit 1 if a copy differs from VERSION (CI, release.sh)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
files=(project.yml Shared/Sources/UnlatchCore/Unlatch.swift Engine/src/ue_version.c Fuse/status.c)
current() {
    case "$1" in
        project.yml) sed -n 's/^    MARKETING_VERSION: "\(.*\)"/\1/p' "$1" ;;
        *Unlatch.swift) sed -n 's/^    public static let version = "\(.*\)"/\1/p' "$1" ;;
        *ue_version.c) sed -n 's/^    return "\(.*\)";/\1/p' "$1" ;;
        *status.c) sed -n 's/^#define UNLATCH_VERSION "\(.*\)"/\1/p' "$1" ;;
    esac
}

if [ "${1:-}" = --check ]; then
    want="$(cat VERSION)" bad=0
    for f in "${files[@]}"; do
        have="$(current "$f")"
        [ "$have" = "$want" ] || { echo "set-version: $f has $have, VERSION has $want" >&2; bad=1; }
    done
    [ $bad = 0 ] && echo "set-version: every copy is $want"
    exit $bad
fi

v="${1:?usage: $0 X.Y.Z[-pre] | --check}"
[[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || { echo "set-version: not a version: $v" >&2; exit 64; }
echo "$v" > VERSION
sed -i '' "s/^    MARKETING_VERSION: \".*\"/    MARKETING_VERSION: \"$v\"/" project.yml
build=$(sed -n 's/^    CURRENT_PROJECT_VERSION: "\(.*\)"/\1/p' project.yml)
sed -i '' "s/^    CURRENT_PROJECT_VERSION: \".*\"/    CURRENT_PROJECT_VERSION: \"$((build + 1))\"/" project.yml
sed -i '' "s/^    public static let version = \".*\"/    public static let version = \"$v\"/" Shared/Sources/UnlatchCore/Unlatch.swift
sed -i '' "s/^    return \".*\";/    return \"$v\";/" Engine/src/ue_version.c
sed -i '' "s/^#define UNLATCH_VERSION \".*\"/#define UNLATCH_VERSION \"$v\"/" Fuse/status.c
"$0" --check
echo "set-version: build $((build + 1)); run Scripts/bootstrap.sh to regenerate the project"
