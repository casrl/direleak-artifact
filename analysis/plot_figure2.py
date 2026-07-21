import csv
import os
import sys

LOOKUP = "UNC_CHA_HITME_LOOKUP.ALL_UMASK"

IN_CSV  = sys.argv[1] if len(sys.argv) > 1 else "output/current/benchmark10_slice09.csv"
OUT_PNG = sys.argv[2] if len(sys.argv) > 2 else "output/figures/figure2_benchmark10.png"

STYLES = [
    ("#111111", "none"),
    ("#888888", "url(#h)"),
    ("#bbbbbb", "none"),
    ("#ffffff", "url(#x)"),
]

FIG2_YMAX = 50000

def load(path):
    series, max_slice = {}, 0
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            if row["event"] != LOOKUP:
                continue
            s   = int(row["socket"])
            dis = int(row["domain_in_socket"])
            ls  = int(row["logical_slice"])
            series.setdefault((s, dis), {})[ls] = float(row["count"])
            max_slice = max(max_slice, ls)
    if not series:
        sys.exit(f"[plot_figure2] no {LOOKUP} rows in {path}")
    slots = sorted(series.keys())
    labels = {k: f"S{k[0] + 1}D{k[1] + 1}" for k in slots}
    return slots, series, max_slice + 1, labels

def home_slice(series, n_slices):
    s1d1 = series.get((0, 0), {})
    return max(range(n_slices), key=lambda ls: s1d1.get(ls, 0.0))

def render_matplotlib(slots, series, n_slices, labels, out_png):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    mstyles = [dict(color=c, hatch=(None if h == "none" else ("//" if "h" in h else "xx")))
               for c, h in STYLES]
    fig, ax = plt.subplots(figsize=(6.6, 3.1))
    x = list(range(n_slices))
    group_w = 0.8
    bar_w = group_w / max(len(slots), 1)
    for i, key in enumerate(slots):
        counts = series[key]
        heights = [counts.get(ls, 0.0) for ls in x]
        offs = [xi - group_w / 2 + bar_w * (i + 0.5) for xi in x]
        st = mstyles[i % len(mstyles)]
        ax.bar(offs, heights, width=bar_w, label=labels[key],
               color=st["color"], edgecolor="black", linewidth=0.4, hatch=st["hatch"])
    ax.set_ylim(0, FIG2_YMAX)
    ax.set_xlabel("Logical Slice Identifier")
    ax.set_ylabel("HITME_LOOKUP\nEvent Count")
    ax.set_xticks(x)
    ax.set_xticklabels([str(ls + 1) for ls in x], fontsize=7)
    ax.set_xlim(-0.6, n_slices - 0.4)
    ax.legend(ncol=len(slots), loc="upper right", fontsize=8, framealpha=0.9)
    ax.set_title("Per-slice HITME_LOOKUP across domains (block homed in S1D1, slice %d)"
                 % (home_slice(series, n_slices) + 1), fontsize=9)
    fig.tight_layout()
    os.makedirs(os.path.dirname(out_png) or ".", exist_ok=True)
    fig.savefig(os.path.splitext(out_png)[0] + ".svg")
    fig.savefig(out_png, dpi=150)
    return out_png

def render_svg(slots, series, n_slices, labels, out_png):
    out = os.path.splitext(out_png)[0] + ".svg"
    W, H = 1100, 380
    ml, mr, mt, mb = 70, 20, 40, 55
    pw, ph = W - ml - mr, H - mt - mb

    top = float(FIG2_YMAX)

    def y(v):
        return mt + ph * (1 - min(v / top, 1.0) if top else 0.0)

    gw = pw / n_slices
    bw = gw * 0.8 / max(len(slots), 1)
    e = []
    e.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'font-family="sans-serif" font-size="11">')
    e.append('<defs>'
             '<pattern id="h" width="4" height="4" patternUnits="userSpaceOnUse" '
             'patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="4" '
             'stroke="#555" stroke-width="1"/></pattern>'
             '<pattern id="x" width="4" height="4" patternUnits="userSpaceOnUse">'
             '<path d="M0,0 L4,4 M4,0 L0,4" stroke="#999" stroke-width="0.7"/></pattern>'
             '</defs>')
    e.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="white"/>')

    nticks = 5
    for t in range(nticks + 1):
        val = top * t / nticks
        yy = y(val)
        e.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{ml+pw}" y2="{yy:.1f}" '
                 f'stroke="#e5e5e5"/>')
        e.append(f'<text x="{ml-8}" y="{yy+4:.1f}" text-anchor="end">{val:g}</text>')
    e.append(f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{mt+ph}" stroke="#333"/>')
    e.append(f'<line x1="{ml}" y1="{mt+ph}" x2="{ml+pw}" y2="{mt+ph}" stroke="#333"/>')

    y0 = mt + ph
    for gi in range(n_slices):
        gx = ml + gi * gw
        for bi, key in enumerate(slots):
            v = series[key].get(gi, 0.0)
            bx = gx + gw * 0.1 + bi * bw
            by = y(v)
            fill, hatch = STYLES[bi % len(STYLES)]
            e.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" '
                     f'height="{y0-by:.1f}" fill="{fill}" stroke="black" '
                     f'stroke-width="0.4"/>')
            if hatch != "none":
                e.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" '
                         f'height="{y0-by:.1f}" fill="{hatch}" stroke="black" '
                         f'stroke-width="0.4"/>')
        e.append(f'<text x="{gx+gw/2:.1f}" y="{y0+14:.1f}" text-anchor="middle" '
                 f'font-size="8">{gi+1}</text>')

    e.append(f'<text x="{ml+pw/2:.1f}" y="{H-6}" text-anchor="middle">'
             f'Logical Slice Identifier</text>')
    e.append(f'<text x="16" y="{mt+ph/2:.1f}" text-anchor="middle" '
             f'transform="rotate(-90 16 {mt+ph/2:.1f})">HITME_LOOKUP Event Count</text>')

    lx = ml + pw - len(slots) * 90
    for i, key in enumerate(slots):
        fill, _ = STYLES[i % len(STYLES)]
        x0 = lx + i * 90
        e.append(f'<rect x="{x0}" y="{mt-22}" width="12" height="12" fill="{fill}" '
                 f'stroke="black" stroke-width="0.5"/>')
        e.append(f'<text x="{x0+16}" y="{mt-12}">{labels[key]}</text>')

    e.append(f'<text x="{ml}" y="{mt-8}" font-size="10" fill="#444">'
             f'block homed in S1D1, slice {home_slice(series, n_slices)+1}</text>')
    e.append('</svg>')

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as fh:
        fh.write("\n".join(e))
    return out

def main():
    slots, series, n_slices, labels = load(IN_CSV)
    try:
        path = render_matplotlib(slots, series, n_slices, labels, OUT_PNG)
        kind = "PNG (matplotlib)"
    except ImportError:
        path = render_svg(slots, series, n_slices, labels, OUT_PNG)
        kind = "SVG (matplotlib unavailable)"
    print(f"[plot_figure2] rendered {kind}")
    print(f"WROTE {path}")

if __name__ == "__main__":
    main()
