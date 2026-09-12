import csv
import os
import sys

IN_CSV  = sys.argv[1] if len(sys.argv) > 1 else "output/current/covert_trace.csv"
OUT_PNG = sys.argv[2] if len(sys.argv) > 2 else "output/figures/covert_channel.png"

def load(path):
    rows = []
    for r in csv.DictReader(open(path, newline="")):
        rows.append(dict(win=int(r["win"]), phase=r["phase"],
                         bt=int(r["bit_true"]), tx=int(r["tx_hits"]),
                         bd=int(r["bd_hits"]), dec=int(r["bit_decoded"]),
                         thr=float(r["thr"]), W=int(r["W"])))
    if not rows:
        sys.exit(f"[plot_covert] no rows in {path}")
    return rows

def score(rows):
    pay = [r for r in rows if r["phase"] == "payload"]
    ok  = sum(1 for r in pay if r["dec"] == r["bt"])
    sent = "".join(str(r["bt"])  for r in pay)
    deco = "".join(str(r["dec"]) for r in pay)
    acc  = 100.0 * ok / len(pay) if pay else 0.0
    return sent, deco, acc, len(pay)

def eviction_signal(rows):
    """Map the per-bit probe measurement into a paper-style eviction trace.

    A transmitted 1 allocates in the transmission set and evicts the Spy's primed
    lines (high signal); a 0 leaves them resident (low signal).  We plot the number
    of evicted lines relative to the primed baseline, so — as in Figure 12 —
    HIGH = evicted = 1 and LOW = survives = 0."""
    thr  = rows[0]["thr"]
    base = max([r["tx"] for r in rows] + [2.0 * thr, 1.0])   # primed / survive level
    tx   = [max(0.0, base - r["tx"]) for r in rows]          # transmission-set eviction
    bd   = [max(0.0, base - r["bd"]) for r in rows]          # boundary-set eviction
    return tx, bd, base - thr, base

def render_matplotlib(rows, out_png):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch

    xs = [r["win"] for r in rows]
    tx, bd, ethr, R = eviction_signal(rows)
    sent, deco, acc, npay = score(rows)
    C1, C0 = "#b3261e", "#1d4e89"          # colours for a 1-window and a 0-window

    fig, ax = plt.subplots(figsize=(8.4, 4.3))

    # one shaded band per one-bit window, colour-coded by the transmitted bit,
    # with the sent bit printed inside the band (as the paper labels each band).
    for r in rows:
        ax.axvspan(r["win"] - 0.5, r["win"] + 0.5,
                   color=("#f7c9c0" if r["bt"] == 1 else "#cddcf0"), alpha=0.6, lw=0)
        ax.text(r["win"], R * 1.17, str(r["bt"]), ha="center", va="center",
                fontsize=9, fontweight="bold", color=(C1 if r["bt"] == 1 else C0))
    ax.text(xs[0] - 1.5, R * 1.17, "sent:", ha="right", va="center",
            fontsize=8, color="#444")

    # boundary set marks each window; transmission set carries the bit
    ax.step(xs, bd, where="mid", color="#9a9a9a", lw=1.0, ls=":", marker="s",
            ms=2.4, alpha=0.85, label="boundary set (marks each window)")
    ax.step(xs, tx, where="mid", color="#2b5fb0", lw=2.0, marker="o", ms=4.0,
            label="transmission set (carries the bit)")
    ax.axhline(ethr, color="#8c1d1f", ls="--", lw=1.3, label="decode threshold")

    # the decision that makes a 0 or a 1: two clearly-labelled signal levels
    xr = xs[-1] + 0.7
    ax.axhline(R, color=C1, ls=":", lw=0.8, alpha=0.5)
    ax.axhline(0, color=C0, ls=":", lw=0.8, alpha=0.5)
    ax.annotate("evicted → 1", (xr, R), fontsize=9, color=C1, va="center",
                fontweight="bold")
    ax.annotate("survives → 0", (xr, 0), fontsize=9, color=C0, va="center",
                fontweight="bold")

    # decoded bit-stream printed under the axis (green = correct, red = wrong)
    for r in rows:
        good = (r["dec"] == r["bt"])
        ax.annotate(str(r["dec"]), (r["win"], -R * 0.13), ha="center", va="top",
                    fontsize=9, color=("#1a7d3c" if good else "#c0161d"),
                    fontweight=("normal" if good else "bold"))
    ax.annotate("decoded:", (xs[0] - 1.5, -R * 0.13), ha="right", va="top",
                fontsize=8, color="#444")

    ax.set_xlabel("Probe iteration (one-bit transmission window)")
    ax.set_ylabel("Evicted MD lines (per bit)")
    ax.set_ylim(-R * 0.26, R * 1.32)
    ax.set_xlim(xs[0] - 2.4, xs[-1] + 4.0)
    ax.set_yticks([0, R])
    ax.set_yticklabels(["0", f"{R:.0f}"], fontsize=8)
    ax.set_title("DireLeak covert channel: transmitting '%s'  —  payload %.0f%% (%d/%d)\n"
                 "each shaded band = one bit · transmission set evicted → 1, survives → 0"
                 % (sent, acc, round(acc / 100 * npay), npay), fontsize=10.5)
    handles, _ = ax.get_legend_handles_labels()
    handles += [Patch(facecolor="#f7c9c0", alpha=0.8, label="window sending 1"),
                Patch(facecolor="#cddcf0", alpha=0.8, label="window sending 0")]
    ax.legend(handles=handles, fontsize=8, loc="upper center",
              bbox_to_anchor=(0.5, -0.20), ncol=3, framealpha=0.0)
    ax.grid(True, axis="y", ls=":", alpha=0.3)
    fig.tight_layout()
    os.makedirs(os.path.dirname(out_png) or ".", exist_ok=True)
    fig.savefig(os.path.splitext(out_png)[0] + ".svg", bbox_inches="tight")
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    return out_png

def render_svg(rows, out_png):
    out = os.path.splitext(out_png)[0] + ".svg"
    xs  = [r["win"] for r in rows]
    tx, bd, ethr, R = eviction_signal(rows)
    ymax = max(2.0, R * 1.15)
    sent, deco, acc, npay = score(rows)
    Wd, Ht = 900, 430
    ml, mr, mt, mb = 64, 20, 54, 66
    pw, ph = Wd - ml - mr, Ht - mt - mb
    xmin, xmax = xs[0], xs[-1]

    def X(x): return ml + pw * (x - xmin) / max(1, xmax - xmin)
    def Y(y): return mt + ph * (1 - y / ymax)

    e = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{Wd}" height="{Ht}" '
         f'font-family="sans-serif" font-size="11">',
         f'<rect width="{Wd}" height="{Ht}" fill="white"/>']
    dx = pw / max(1, xmax - xmin)
    for r in rows:
        col = "#f7c9c0" if r["bt"] == 1 else "#cddcf0"
        e.append(f'<rect x="{X(r["win"])-dx/2:.1f}" y="{mt}" width="{dx:.1f}" '
                 f'height="{ph}" fill="{col}" opacity="0.6"/>')
        e.append(f'<text x="{X(r["win"]):.1f}" y="{mt-4:.1f}" text-anchor="middle" '
                 f'font-size="9" font-weight="bold" '
                 f'fill="{"#b3261e" if r["bt"]==1 else "#1d4e89"}">{r["bt"]}</text>')
    e.append(f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{mt+ph}" stroke="#333"/>')
    e.append(f'<line x1="{ml}" y1="{mt+ph}" x2="{ml+pw}" y2="{mt+ph}" stroke="#333"/>')
    e.append(f'<line x1="{ml}" y1="{Y(ethr):.1f}" x2="{ml+pw}" y2="{Y(ethr):.1f}" '
             f'stroke="#8c1d1f" stroke-dasharray="6,3"/>')
    e.append(f'<text x="{ml+pw-4}" y="{Y(ethr)-4:.1f}" text-anchor="end" '
             f'fill="#8c1d1f" font-size="10">decode threshold</text>')
    for series, col, r_ in ((bd, "#9a9a9a", 2.0), (tx, "#2b5fb0", 2.6)):
        pts = " ".join(f"{X(x):.1f},{Y(v):.1f}" for x, v in zip(xs, series))
        e.append(f'<polyline points="{pts}" fill="none" stroke="{col}" stroke-width="1.7"/>')
        for x, v in zip(xs, series):
            e.append(f'<circle cx="{X(x):.1f}" cy="{Y(v):.1f}" r="{r_}" fill="{col}"/>')
    e.append(f'<text x="{ml+pw-4:.1f}" y="{Y(R)+4:.1f}" text-anchor="end" '
             f'fill="#b3261e" font-size="10" font-weight="bold">evicted → 1</text>')
    e.append(f'<text x="{ml+pw-4:.1f}" y="{Y(0)-4:.1f}" text-anchor="end" '
             f'fill="#1d4e89" font-size="10" font-weight="bold">survives → 0</text>')
    for r in rows:
        col = "#1a7d3c" if r["dec"] == r["bt"] else "#c0161d"
        e.append(f'<text x="{X(r["win"]):.1f}" y="{mt+ph+16:.1f}" text-anchor="middle" '
                 f'font-size="9" fill="{col}">{r["dec"]}</text>')
    e.append(f'<text x="{ml+pw/2:.1f}" y="{Ht-6}" text-anchor="middle">'
             f'probe iteration (one-bit window) — decoded bits below axis</text>')
    e.append(f'<text x="16" y="{mt+ph/2:.1f}" text-anchor="middle" '
             f'transform="rotate(-90 16 {mt+ph/2:.1f})">Evicted MD lines (per bit)</text>')
    e.append(f'<text x="{ml}" y="20" font-size="13">DireLeak covert channel: '
             f"transmitting '{sent}' — payload {acc:.0f}%  "
             f'(evicted → 1, survives → 0)</text>')
    e.append('</svg>')
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "w").write("\n".join(e))
    return out

def main():
    rows = load(IN_CSV)
    # show only the payload bits (e.g. 01101001); drop the calibration preamble.
    pay = [r for r in rows if r["phase"] == "payload"] or rows
    for i, r in enumerate(pay):
        r["win"] = i
    sent, deco, acc, npay = score(pay)
    try:
        path = render_matplotlib(pay, OUT_PNG); kind = "PNG+SVG (matplotlib)"
    except ImportError:
        path = render_svg(pay, OUT_PNG); kind = "SVG (matplotlib unavailable)"
    print(f"[plot_covert] sent='{sent}' decoded='{deco}' payload accuracy={acc:.1f}% ({npay} bits)")
    print(f"[plot_covert] rendered {kind}")
    print(f"WROTE {path}")

if __name__ == "__main__":
    main()
