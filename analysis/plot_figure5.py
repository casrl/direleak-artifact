import csv
import os
import sys

IN_CSV  = sys.argv[1] if len(sys.argv) > 1 else "output/current/benchmark11_fig5.csv"
OUT_PNG = sys.argv[2] if len(sys.argv) > 2 else "output/figures/figure5_benchmark11.png"

NBINS = 60

def load(path):
    hit, miss = [], []
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            v = float(row["latency_cycles"])
            (hit if row["scenario"] == "hit" else miss).append(v)
    if not hit or not miss:
        sys.exit(f"[plot_figure5] need both hit and miss samples in {path}")
    return sorted(hit), sorted(miss)

def pct(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    i = min(len(sorted_vals) - 1, max(0, int(round(p / 100.0 * (len(sorted_vals) - 1)))))
    return sorted_vals[i]

def histogram(vals, lo, hi, nbins):
    width = (hi - lo) / nbins if hi > lo else 1.0
    counts = [0] * nbins
    for v in vals:
        b = int((v - lo) / width)
        if 0 <= b < nbins:
            counts[b] += 1
    n = len(vals)
    centers = [lo + (b + 0.5) * width for b in range(nbins)]
    freq = [100.0 * c / n for c in counts]
    return centers, freq

def axis_range(hit, miss):
    lo = min(pct(hit, 0.5), pct(miss, 0.5))
    hi = max(pct(hit, 99.5), pct(miss, 99.5))
    pad = 0.05 * (hi - lo if hi > lo else 1.0)
    return lo - pad, hi + pad

def render_matplotlib(hit, miss, out_png):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    lo, hi = axis_range(hit, miss)
    hc, hf = histogram(hit, lo, hi, NBINS)
    mc, mf = histogram(miss, lo, hi, NBINS)

    fig, ax = plt.subplots(figsize=(5.2, 3.4))

    ax.fill_between(hc, hf, color="#4C72B0", alpha=0.45, label="Hit")
    ax.plot(hc, hf, color="#1f3b73", lw=1.4)
    ax.fill_between(mc, mf, color="#C44E52", alpha=0.40, hatch="///", label="Miss")
    ax.plot(mc, mf, color="#8c1d1f", lw=1.4, ls="--")
    ax.set_xlabel("Latency (Cycles)")
    ax.set_ylabel("Frequency (%)")
    ax.set_xlim(lo, hi)
    ax.set_ylim(bottom=0)
    ax.legend(loc="upper right", fontsize=9)
    ax.set_title("MD cache hit vs miss access latency (SPR)", fontsize=10)
    fig.tight_layout()
    os.makedirs(os.path.dirname(out_png) or ".", exist_ok=True)
    fig.savefig(os.path.splitext(out_png)[0] + ".svg")
    fig.savefig(out_png, dpi=150)
    return out_png

def render_svg(hit, miss, out_png):
    out = os.path.splitext(out_png)[0] + ".svg"
    lo, hi = axis_range(hit, miss)
    hc, hf = histogram(hit, lo, hi, NBINS)
    mc, mf = histogram(miss, lo, hi, NBINS)
    fmax = max(max(hf), max(mf), 1e-9)

    W, H = 640, 400
    ml, mr, mt, mb = 60, 20, 30, 50
    pw, ph = W - ml - mr, H - mt - mb

    def X(v):
        return ml + pw * (v - lo) / (hi - lo if hi > lo else 1.0)

    def Y(f):
        return mt + ph * (1 - f / fmax)

    def poly(centers, freq):
        return " ".join(f"{X(c):.1f},{Y(f):.1f}" for c, f in zip(centers, freq))

    e = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'font-family="sans-serif" font-size="11">']
    e.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="white"/>')
    e.append(f'<text x="{ml + pw / 2}" y="18" text-anchor="middle" font-size="12">'
             f'MD cache hit vs miss access latency (SPR)</text>')

    e.append(f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{mt+ph}" stroke="#333"/>')
    e.append(f'<line x1="{ml}" y1="{mt+ph}" x2="{ml+pw}" y2="{mt+ph}" stroke="#333"/>')
    for t in range(6):
        xv = lo + (hi - lo) * t / 5
        xx = X(xv)
        e.append(f'<line x1="{xx:.1f}" y1="{mt+ph}" x2="{xx:.1f}" y2="{mt+ph+4}" stroke="#333"/>')
        e.append(f'<text x="{xx:.1f}" y="{mt+ph+16}" text-anchor="middle" font-size="9">{xv:.0f}</text>')
    for t in range(5):
        fv = fmax * t / 4
        yy = Y(fv)
        e.append(f'<line x1="{ml-4}" y1="{yy:.1f}" x2="{ml}" y2="{yy:.1f}" stroke="#333"/>')
        e.append(f'<text x="{ml-6}" y="{yy+3:.1f}" text-anchor="end" font-size="9">{fv:.1f}</text>')

    e.append(f'<polyline points="{poly(hc, hf)}" fill="none" stroke="#1f77b4" stroke-width="1.8"/>')
    e.append(f'<polyline points="{poly(mc, mf)}" fill="none" stroke="#d62728" '
             f'stroke-width="1.8" stroke-dasharray="5,3"/>')

    e.append(f'<text x="{ml+pw/2:.1f}" y="{H-8}" text-anchor="middle">Latency (Cycles)</text>')
    e.append(f'<text x="16" y="{mt+ph/2:.1f}" text-anchor="middle" '
             f'transform="rotate(-90 16 {mt+ph/2:.1f})">Frequency (%)</text>')

    e.append(f'<line x1="{ml+pw-120}" y1="{mt+6}" x2="{ml+pw-100}" y2="{mt+6}" stroke="#1f77b4" stroke-width="2"/>')
    e.append(f'<text x="{ml+pw-96}" y="{mt+9}">Hit</text>')
    e.append(f'<line x1="{ml+pw-120}" y1="{mt+22}" x2="{ml+pw-100}" y2="{mt+22}" stroke="#d62728" stroke-width="2" stroke-dasharray="5,3"/>')
    e.append(f'<text x="{ml+pw-96}" y="{mt+25}">Miss</text>')
    e.append('</svg>')

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as fh:
        fh.write("\n".join(e))
    return out

def trim(sorted_vals, frac):
    n = len(sorted_vals)
    lo, hi = int(n * frac), n - int(n * frac)
    return sorted_vals[lo:hi] if hi > lo else sorted_vals

def main():
    hit, miss = load(IN_CSV)
    TRIM = float(os.environ.get("FIG5_TRIM", "0.2"))
    if TRIM > 0:
        raw_h, raw_m = len(hit), len(miss)
        hit, miss = trim(hit, TRIM), trim(miss, TRIM)
        print(f"[plot_figure5] trimmed {TRIM:.0%}/{TRIM:.0%} per scenario: "
              f"hit {raw_h}->{len(hit)}, miss {raw_m}->{len(miss)}")
    try:
        path = render_matplotlib(hit, miss, OUT_PNG)
        kind = "PNG+SVG (matplotlib)"
    except ImportError:
        path = render_svg(hit, miss, OUT_PNG)
        kind = "SVG (matplotlib unavailable)"
    hm = hit[len(hit) // 2]
    mm = miss[len(miss) // 2]
    hmean = sum(hit) / len(hit)
    mmean = sum(miss) / len(miss)
    print(f"[plot_figure5] rendered {kind}  (hit med={hm:.0f} mean={hmean:.0f}, "
          f"miss med={mm:.0f} mean={mmean:.0f}, "
          f"med-gap={mm - hm:.0f} mean-gap={mmean - hmean:.0f} cyc)")
    print(f"WROTE {path}")

if __name__ == "__main__":
    main()
