# SPDX-License-Identifier: GPL-3.0-or-later
# Turns make-windows-golden.ps1's manifest.tsv (what Windows reports) into a golden .expect
# file for Scripts/test-golden.sh. Link targets follow D-49/D-67: relative targets read back
# with '/' separators; absolute targets on this volume (W:) are shown under the mount point,
# which the harness passes as /mnt. Fields are tab-separated (names contain spaces).
import sys

manifest, windows = sys.argv[1], sys.argv[2]
attr_flag = {"Hidden": "hidden", "ReadOnly": "readonly", "System": "system",
             "Compressed": "compressed", "SparseFile": "sparse"}
out = [f"# Made by Windows ({windows}) with Tests/Images/windows/make.sh; expectations from",
       "# Windows' own report (manifest.tsv), not from Unlatch.",
       "state clean"]
for line in open(manifest, encoding="utf-8"):
    f = line.rstrip("\r\n").split("\t")      # Windows writes CRLF
    kind = f[0]
    if kind == "label":
        out.append(f"label\t{f[1]}")
    elif kind == "count":
        out.append(f"count\t{f[1]}\t{f[2]}")
    elif kind == "file":
        out.append(f"file\t{f[1]}\t{f[2]}\t{f[3]}")
    elif kind == "attr":
        out.append(f"flag\t{f[1]}\t{attr_flag[f[2]]}")
    elif kind == "stream":
        out.append(f"stream\t{f[1]}\t{f[2]}\t{f[3]}\t{f[4]}")
    elif kind == "link":
        path, target = f[1], f[3]
        if len(target) > 2 and target[1] == ":":            # absolute, on this volume (W:)
            target = "/mnt/" + target[3:].replace("\\", "/")
        else:
            target = target.replace("\\", "/")
        out.append(f"link\t{path}\t{target}")
out.append("rw_open 0")
print("\n".join(out))
