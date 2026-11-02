#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Draws the real-world copy chart in the README (a GoPro footage folder copied in Finder to a
# Seagate USB 3.0 hard drive) from Bench/results/*-unlatch-seagate-gopro.json:
# Bench/seagate.svg and Bench/seagate-dark.svg.
#
#   Bench/seagate-chart.py [--from-log LOG]
#
# LOG has one "<unix seconds> <KiB used>" line per sample (df -k of the mounted volume every
# 5 s during the copy); --from-log turns it into the results file first.
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent.parent
results = root / "Bench" / "results"
LABEL = "unlatch-seagate-gopro"
SMOOTH = 6  # samples in the moving average (30 s at 5 s per sample)
COPY_START = 1793609444  # birth time of the copied folder on the drive (stat -f %B)
COPY_GB = 120261280 * 1024 / 1e9  # du -sk of the source folder


def from_log(log):
    rows = [tuple(map(int, l.split())) for l in pathlib.Path(log).read_text().split("\n") if l.strip()]
    # Drop the idle tail after the copy ended.
    while len(rows) > 1 and rows[-1][1] == rows[-2][1]:
        rows.pop()
    t0, u0 = rows[0]
    secs = rows[-1][0] - t0
    gb = (rows[-1][1] - u0) * 1024 / 1e9
    return {
        "label": LABEL,
        "date": "2026-11-02",
        "device": "Seagate Expansion 1 TB portable hard drive (2.5\", 5400 rpm), USB 3.0 (5 Gb/s), NTFS, 34% full",
        "source": "GoPro footage folder on the internal SSD: 364 files, 123 GB, 19 files over 1 GiB",
        "method": "Finder copy; df -k of the mounted volume every 5 s (the copy was already running when sampling began)",
        "unlatch": "1.0.0-beta.5, kernel extension backend",
        "rated_mb_per_s": 130,
        # The whole copy: the folder's creation time on the drive to the last sample that grew.
        "copy": {"start": COPY_START, "end": rows[-1][0], "seconds": rows[-1][0] - COPY_START,
                 "gb": round(COPY_GB, 2), "mb_per_s": round(COPY_GB * 1000 / (rows[-1][0] - COPY_START), 1)},
        "sampled": {"seconds": secs, "gb": round(gb, 2), "mb_per_s": round(gb * 1000 / secs, 1)},
        "samples": [[t - t0, u - u0] for t, u in rows],
    }


if len(sys.argv) == 3 and sys.argv[1] == "--from-log":
    data = from_log(sys.argv[2])
    path = results / f"{data['date']}-{LABEL}.json"
    path.write_text(json.dumps(data, indent=2) + "\n")
    print(path.relative_to(root))

data = json.loads(sorted(results.glob(f"*-{LABEL}.json"))[-1].read_text())
s = data["samples"]
# Sampling began after the copy did: place samples on the copy's own clock.
lead = data["copy"]["end"] - s[-1][0] - data["copy"]["start"]
# The last interval ends mid-way through it: the copy finished inside it.
rates = [((s[i][1] - s[i - 1][1]) * 1024 / 1e6 / (s[i][0] - s[i - 1][0]), s[i][0] + lead)
         for i in range(1, len(s) - 1)]
smooth = []
for i in range(len(rates)):
    win = rates[max(0, i - SMOOTH + 1):i + 1]
    smooth.append((rates[i][1] / 60, sum(r for r, _ in win) / len(win)))
avg, rated = data["copy"]["mb_per_s"], data["rated_mb_per_s"]

THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", base="#c3c2b7", grid="#e6e5df",
                  device="#1baf7a", unlatch="#2a78d6"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", base="#383835", grid="#2b2b29",
                 device="#199e70", unlatch="#3987e5"),
}
W, H = 800, 300
L, R, T, B = 46, 190, 40, 36  # plot margins
FONT = "system-ui, -apple-system, 'Segoe UI', sans-serif"


def svg(t):
    pw, ph = W - L - R, H - T - B
    xmax = data["copy"]["seconds"] / 60
    ymax = 160
    X = lambda m: L + pw * m / xmax
    Y = lambda v: T + ph * (1 - v / ymax)
    out = [f'<text x="0" y="16" fill="{t["ink"]}" font-size="14" font-weight="600">Write speed during the copy '
           f'<tspan fill="{t["ink2"]}" font-weight="400">(MB/s, 30 s average)</tspan></text>']
    for v in range(0, ymax + 1, 40):
        out.append(f'<line x1="{L}" y1="{Y(v):.1f}" x2="{L + pw}" y2="{Y(v):.1f}" stroke="{t["grid"]}"/>')
        out.append(f'<text x="{L - 8}" y="{Y(v) + 4:.1f}" text-anchor="end" fill="{t["ink2"]}" font-size="12" '
                   f'font-variant-numeric="tabular-nums">{v}</text>')
    step = 5 if xmax > 12 else 2
    for m in range(0, int(xmax) - 1, step):
        out.append(f'<text x="{X(m):.1f}" y="{T + ph + 18}" text-anchor="middle" fill="{t["ink2"]}" '
                   f'font-size="12">{m} min</text>')
    out.append(f'<line x1="{L}" y1="{T + ph}" x2="{L + pw}" y2="{T + ph}" stroke="{t["base"]}"/>')
    out.append(f'<text x="{X(lead / 120):.1f}" y="{Y(60):.1f}" text-anchor="middle" fill="{t["ink2"]}" '
               f'font-size="12" font-style="italic">not sampled</text>')
    out.append(f'<text x="{X(xmax):.1f}" y="{T + ph + 18}" text-anchor="end" fill="{t["ink2"]}" '
               f'font-size="12">done, {xmax:.0f} min</text>')
    # Reference lines, labelled at the right edge.
    for v, color, name in [(rated, t["device"], f"Drive's rating: up to {rated}"),
                           (avg, t["unlatch"], f"Unlatch average: {avg:.0f}")]:
        out.append(f'<line x1="{L}" y1="{Y(v):.1f}" x2="{L + pw}" y2="{Y(v):.1f}" stroke="{color}" '
                   f'stroke-width="1.5" stroke-dasharray="5 4"/>')
        out.append(f'<text x="{L + pw + 8}" y="{Y(v) + 4:.1f}" fill="{color}" font-size="12" '
                   f'font-weight="600">{name}</text>')
    pts = " ".join(f"{X(m):.1f},{Y(v):.1f}" for m, v in smooth)
    out.append(f'<polyline points="{pts}" fill="none" stroke="{t["unlatch"]}" stroke-width="2" '
               f'stroke-linejoin="round"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="-12 -4 {W + 24} {H + 8}" '
            f'font-family="{FONT}" role="img" aria-label="Write speed over a {data["copy"]["gb"]:.0f} GB Finder copy '
            f'to a Seagate USB 3.0 hard drive through Unlatch: {avg:.0f} MB/s average against the drive\'s rated '
            f'{rated} MB/s">\n'
            f'<rect x="-12" y="-4" width="{W + 24}" height="{H + 8}" rx="8" fill="{t["surface"]}"/>\n'
            + "\n".join(out) + "\n</svg>\n")


for name, theme in THEMES.items():
    path = root / "Bench" / ("seagate.svg" if name == "light" else "seagate-dark.svg")
    path.write_text(svg(theme))
    print(path.relative_to(root))
