#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Markdown table of Bench/results/*.json (plan Step 11).

    Bench/compare.py [results.json ...]     default: the newest result per label

Each column is one label; ratios are against the label given with --base (default apfs).
"""
import argparse
import glob
import json
import os

ROWS = [
    ("Sequential write", "mb_per_s", "seq_write", "MB/s"),
    ("Sequential read, cold", "mb_per_s", "seq_read_cold", "MB/s"),
    ("Sequential read, warm", "mb_per_s", "seq_read_warm", "MB/s"),
    ("Raw device read", "mb_per_s", "raw_read", "MB/s"),
    ("Small files (1-16 KiB)", "small_files_per_s", None, "files/s"),
    ("Metadata (create, rename, delete)", "metadata_ops_per_s", None, "ops/s"),
]


def value(r, group, key):
    v = r.get(group)
    if key is not None:
        v = v.get(key) if isinstance(v, dict) else None
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--base", default="apfs")
    ap.add_argument("--order", default="apfs,exfat,ntfs3g,unlatch-file,unlatch")
    args = ap.parse_args()
    root = os.path.dirname(os.path.abspath(__file__))
    files = args.files or sorted(glob.glob(os.path.join(root, "results", "*.json")))
    by_label = {}
    for f in files:                       # sorted by name = by date: the newest wins
        with open(f) as fh:
            r = json.load(fh)
        by_label[r["label"]] = r
    order = [l for l in args.order.split(",") if l in by_label] + \
            sorted(l for l in by_label if l not in args.order.split(","))
    base = by_label.get(args.base)
    print("| Test | " + " | ".join(order) + " |")
    print("|---|" + "---:|" * len(order))
    for name, group, key, unit in ROWS:
        cells = []
        for l in order:
            v = value(by_label[l], group, key)
            if v is None:
                cells.append("–")
                continue
            cell = f"{v:,.0f} {unit}"
            b = value(base, group, key) if base else None
            if b and l != args.base:
                cell += f" ({100 * v / b:.0f}%)"
            cells.append(cell)
        print(f"| {name} | " + " | ".join(cells) + " |")
    any_r = next(iter(by_label.values()))
    print()
    print(f"{any_r['machine']}, macOS {any_r['macos']}, commit {any_r['commit']}; "
          f"median of {any_r['runs']} runs, {any_r['mib']} MiB file, {any_r['small_files']} small files, "
          f"{any_r['metadata_files']} metadata files; cold reads: {any_r.get('cold_reads', '?')}. "
          f"Percentages are of {args.base}.")


if __name__ == "__main__":
    main()
