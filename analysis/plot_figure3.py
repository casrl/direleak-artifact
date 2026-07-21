import csv
import math
import os
import sys

IN_CSV  = sys.argv[1] if len(sys.argv) > 1 else "output/current/figure3_matrix.csv"
OUT_PNG = sys.argv[2] if len(sys.argv) > 2 else "output/figures/figure3_benchmark10.png"

def load(path):
    cells = {}
    n = 0
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            t = int(row["target_slice"])
            o = int(row["observed_slice"])
            cells[(t, o)] = float(row["count"])
            n = max(n, t + 1, o + 1)
    if not cells:
        sys.exit(f"[plot_figure3] no rows in {path}")
    grid = [[cells.get((t, o), 0.0) for o in range(n)] for t in range(n)]
    return grid, n

def render_matplotlib(grid, n, out_png):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LogNorm

    norm = LogNorm(vmin=1e3, vmax=1e5)

    fig, ax = plt.subplots(figsize=(9, 8))
    im = ax.imshow(grid, cmap="gray_r", norm=norm, aspect="equal", origin="upper")
    ax.set_xlabel("CHA Slice Identifier (observed)")
    ax.set_ylabel("Home Slice Identifier (accessed)")
    ax.set_xticks(range(n)); ax.set_xticklabels([str(i + 1) for i in range(n)], fontsize=6)
    ax.set_yticks(range(n)); ax.set_yticklabels([str(i + 1) for i in range(n)], fontsize=6)
    ax.set_title("HitME slice-mapping correlation matrix (home domain S1D1)", fontsize=10)

    for i in range(n):
        v = grid[i][i]
        if v > 0:
            ax.text(i, i, f"{v:.0f}", ha="center", va="center", fontsize=5, color="white")
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04, label="HITME_LOOKUP Event Count")
    fig.tight_layout()
    os.makedirs(os.path.dirname(out_png) or ".", exist_ok=True)
    fig.savefig(os.path.splitext(out_png)[0] + ".svg")
    fig.savefig(out_png, dpi=150)
    return out_png

def render_svg(grid, n, out_png):
    out = os.path.splitext(out_png)[0] + ".svg"
    cell = 26
    ml, mt = 46, 46
    mr, mb = 30, 40
    W = ml + n * cell + mr
    H = mt + n * cell + mb

    LO, HI = 3.0, 5.0

    def shade(v):
        if v <= 0:
            return "#ffffff"
        t = (math.log10(v) - LO) / (HI - LO)
        t = max(0.0, min(1.0, t))
        g = int(round(255 * (1.0 - t)))
        return f"#{g:02x}{g:02x}{g:02x}"

    e = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'font-family="sans-serif" font-size="10">']
    e.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="white"/>')
    e.append(f'<text x="{ml + n*cell/2}" y="18" text-anchor="middle" font-size="12">'
             f'HitME slice-mapping correlation matrix (S1D1)</text>')
    for t in range(n):
        for o in range(n):
            v = grid[t][o]
            x = ml + o * cell
            y = mt + t * cell
            e.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" '
                     f'fill="{shade(v)}" stroke="#ddd" stroke-width="0.5"/>')
            if t == o and v > 0:
                e.append(f'<text x="{x+cell/2:.1f}" y="{y+cell/2+3:.1f}" '
                         f'text-anchor="middle" font-size="6" fill="white">{v:.0f}</text>')

    for i in range(n):
        e.append(f'<text x="{ml + i*cell + cell/2:.1f}" y="{mt-6}" text-anchor="middle" '
                 f'font-size="7">{i+1}</text>')
        e.append(f'<text x="{ml-6}" y="{mt + i*cell + cell/2+3:.1f}" text-anchor="end" '
                 f'font-size="7">{i+1}</text>')
    e.append(f'<text x="{ml + n*cell/2:.1f}" y="{H-8}" text-anchor="middle">'
             f'CHA Slice Identifier (observed)</text>')
    e.append(f'<text x="14" y="{mt + n*cell/2:.1f}" text-anchor="middle" '
             f'transform="rotate(-90 14 {mt + n*cell/2:.1f})">'
             f'Home Slice Identifier (accessed)</text>')
    e.append('</svg>')

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as fh:
        fh.write("\n".join(e))
    return out

def main():
    grid, n = load(IN_CSV)
    try:
        path = render_matplotlib(grid, n, OUT_PNG)
        kind = "PNG (matplotlib)"
    except ImportError:
        path = render_svg(grid, n, OUT_PNG)
        kind = "SVG (matplotlib unavailable)"
    print(f"[plot_figure3] rendered {kind}")
    print(f"WROTE {path}")

if __name__ == "__main__":
    main()
