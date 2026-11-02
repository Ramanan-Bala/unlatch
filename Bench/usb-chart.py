#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Draws the USB stick benchmark chart in the README from Bench/results/*-usb-sandisk.json:
# Bench/usb.svg and Bench/usb-dark.svg.
#
#   Bench/usb-chart.py
import json
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
results = root / "Bench" / "results"


def newest(label):
    return json.loads(sorted(results.glob(f"*-{label}.json"))[-1].read_text())


raw, unlatch, exfat = newest("raw-usb-sandisk"), newest("unlatch-usb-sandisk"), newest("exfat-usb-sandisk")

SERIES = [("device", "Stick itself (raw dd)"), ("unlatch", "Unlatch (NTFS)"), ("exfat", "exFAT (Apple)")]
PANELS = [
    ("Sequential read, cold", "MB/s", {"device": raw["mb_per_s"]["raw_read"],
                                      "unlatch": unlatch["mb_per_s"]["seq_read_cold"],
                                      "exfat": exfat["mb_per_s"]["seq_read_cold"]}),
    ("Sequential write", "MB/s", {"device": raw["mb_per_s"]["raw_write"],
                                  "unlatch": unlatch["mb_per_s"]["seq_write"],
                                  "exfat": exfat["mb_per_s"]["seq_write"]}),
    ("Small files, 1–16 KiB", "files/s", {"unlatch": unlatch["small_files_per_s"],
                                          "exfat": exfat["small_files_per_s"]}),
    ("Metadata: create, rename, delete", "ops/s", {"unlatch": unlatch["metadata_ops_per_s"],
                                                   "exfat": exfat["metadata_ops_per_s"]}),
]

THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", base="#c3c2b7",
                  device="#1baf7a", unlatch="#2a78d6", exfat="#eb6834"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", base="#383835",
                 device="#199e70", unlatch="#3987e5", exfat="#d95926"),
}

W, PANEL_W, GAP_X = 800, 380, 40
LABEL_W, VALUE_W, BAR_H, ROW = 150, 70, 16, 26
FONT = "system-ui, -apple-system, 'Segoe UI', sans-serif"


def fmt(v, unit):
    return f"{v:,.1f}" if unit == "MB/s" else f"{v:,.0f}"


def bar(x, y, w, h, color):
    # 4px rounded end away from the baseline, square at the baseline.
    r = min(4, w / 2, h / 2)
    return (f'<path d="M{x},{y} h{w - r:.1f} a{r},{r} 0 0 1 {r},{r} v{h - 2 * r} '
            f'a{r},{r} 0 0 1 -{r},{r} h-{w - r:.1f} z" fill="{color}"/>')


def svg(t):
    out, y0 = [], 44
    # Legend, one row at the top.
    lx = 0
    for key, name in SERIES:
        out.append(f'<rect x="{lx}" y="8" width="12" height="12" rx="3" fill="{t[key]}"/>')
        out.append(f'<text x="{lx + 18}" y="18" fill="{t["ink2"]}" font-size="13">{name}</text>')
        lx += 30 + 7.2 * len(name)
    panel_h = []
    for i, (title, unit, vals) in enumerate(PANELS):
        col, rowi = i % 2, i // 2
        px = col * (PANEL_W + GAP_X)
        py = y0 + rowi * (36 + 3 * ROW + 24)
        top = max(vals.values())
        plot_w = PANEL_W - LABEL_W - VALUE_W
        out.append(f'<text x="{px}" y="{py + 12}" fill="{t["ink"]}" font-size="14" font-weight="600">'
                   f'{title} <tspan fill="{t["ink2"]}" font-weight="400">({unit})</tspan></text>')
        by = py + 26
        rows = [(k, n) for k, n in SERIES if k in vals]
        for j, (key, name) in enumerate(rows):
            yy = by + j * ROW
            w = max(2, plot_w * vals[key] / top)
            out.append(f'<text x="{px + LABEL_W - 10}" y="{yy + 12.5}" text-anchor="end" '
                       f'fill="{t["ink2"]}" font-size="12">{name}</text>')
            out.append(f'<g><title>{name}: {fmt(vals[key], unit)} {unit}</title>'
                       f'{bar(px + LABEL_W, yy, w, BAR_H, t[key])}</g>')
            out.append(f'<text x="{px + LABEL_W + w + 6:.1f}" y="{yy + 12.5}" fill="{t["ink"]}" '
                       f'font-size="12" font-variant-numeric="tabular-nums">{fmt(vals[key], unit)}</text>')
        out.append(f'<line x1="{px + LABEL_W}" y1="{by - 3}" x2="{px + LABEL_W}" '
                   f'y2="{by + len(rows) * ROW - 7}" stroke="{t["base"]}" stroke-width="1"/>')
        panel_h.append(py + 26 + len(rows) * ROW)
    h = max(panel_h) + 6
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{h}" viewBox="-12 -4 {W + 24} {h + 8}" '
            f'font-family="{FONT}" role="img" aria-label="Unlatch versus exFAT and the raw device on a USB 2.0 stick">\n'
            f'<rect x="-12" y="-4" width="{W + 24}" height="{h + 8}" rx="8" fill="{t["surface"]}"/>\n'
            + "\n".join(out) + "\n</svg>\n")


out_dir = root / "Bench"
for name, theme in THEMES.items():
    path = out_dir / ("usb.svg" if name == "light" else "usb-dark.svg")
    path.write_text(svg(theme))
    print(path.relative_to(root))
