import os
import re
import sys

DEF_A   = "output/current/benchmark13a.log"
DEF_B   = "output/current/benchmark13b.log"
OUT_DEF = "output/figures/obs2_clflush_benchmark13.png"

LOG_A   = sys.argv[1] if len(sys.argv) > 1 else DEF_A
LOG_B   = sys.argv[2] if len(sys.argv) > 2 else DEF_B
OUT_PNG = sys.argv[3] if len(sys.argv) > 3 else OUT_DEF

HOME_CHA = int(os.environ.get("OBS2_SLICE", "9"))
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

def home_hit(path, cha=HOME_CHA, socket=0):
    txt = ANSI.sub("", open(path, errors="replace").read())
    lines = txt.splitlines()
    in_hit = in_p3 = False
    for ln in lines:
        s = ln.strip()
        if s.startswith("Event:"):
            in_hit = "HITME_HIT" in s
            in_p3 = False
            continue
        if in_hit and "Std Dev" in s:
            in_p3 = True
            continue
        if in_hit and in_p3:
            p = s.split()
            if len(p) >= 4 and p[0] == str(socket) and p[1] == str(cha):
                runs = p[2:-2]
                try:
                    return sum(float(x) for x in runs)
                except ValueError:
                    pass
    sys.exit(f"[plot_obs2] could not find raw per-run HITME_HIT for "
             f"socket {socket} CHA {cha} in {path}")

LABELS = ["No flush", "After CLFLUSH"]

def render_matplotlib(hit, out_png):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(4.6, 3.8))
    bars = ax.bar(range(2), hit, 0.55, color=["#4C72B0", "#B34B4B"],
                  edgecolor="#222", linewidth=0.8)
    for r in bars:
        v = r.get_height()
        ax.annotate(f"{v:,.0f}", (r.get_x() + r.get_width() / 2, v),
                    ha="center", va="bottom", fontsize=10, xytext=(0, 2),
                    textcoords="offset points")
    ax.set_xticks(range(2))
    ax.set_xticklabels(LABELS)
    ax.set_ylabel(f"HITME_HIT events  (home slice {HOME_CHA}, total)")
    ax.set_title("CLFLUSH invalidates the MD cache (Obs. 2)", fontsize=11)
    ax.set_ylim(0, max(hit) * 1.18)
    fig.tight_layout()
    os.makedirs(os.path.dirname(out_png) or ".", exist_ok=True)
    fig.savefig(os.path.splitext(out_png)[0] + ".svg")
    fig.savefig(out_png, dpi=150)
    return out_png

def render_svg(hit, out_png):
    out = os.path.splitext(out_png)[0] + ".svg"
    ymax = max(hit) * 1.18 or 1.0
    W, H = 480, 400
    ml, mr, mt, mb = 78, 20, 40, 50
    pw, ph = W - ml - mr, H - mt - mb

    def Y(v):
        return mt + ph * (1 - v / ymax)

    cols = ["#4C72B0", "#B34B4B"]
    e = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'font-family="sans-serif" font-size="11">',
         f'<rect width="{W}" height="{H}" fill="white"/>',
         f'<text x="{ml + pw / 2}" y="20" text-anchor="middle" font-size="13">'
         f'CLFLUSH invalidates the MD cache (Obs. 2)</text>',
         f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{mt+ph}" stroke="#333"/>',
         f'<line x1="{ml}" y1="{mt+ph}" x2="{ml+pw}" y2="{mt+ph}" stroke="#333"/>',
         f'<text x="18" y="{mt+ph/2:.1f}" text-anchor="middle" '
         f'transform="rotate(-90 18 {mt+ph/2:.1f})">HITME_HIT (home slice {HOME_CHA})</text>']
    gw = pw / 2
    for gi in range(2):
        bw = gw * 0.5
        bx = ml + gw * gi + (gw - bw) / 2
        e.append(f'<rect x="{bx:.1f}" y="{Y(hit[gi]):.1f}" width="{bw:.1f}" '
                 f'height="{mt+ph-Y(hit[gi]):.1f}" fill="{cols[gi]}" stroke="#222"/>')
        e.append(f'<text x="{bx+bw/2:.1f}" y="{Y(hit[gi])-4:.1f}" text-anchor="middle">'
                 f'{int(round(hit[gi])):,}</text>')
        e.append(f'<text x="{ml+gw*(gi+0.5):.1f}" y="{mt+ph+16:.1f}" '
                 f'text-anchor="middle">{LABELS[gi]}</text>')
    e.append('</svg>')
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as fh:
        fh.write("\n".join(e))
    return out

def main():
    hit = [home_hit(LOG_A), home_hit(LOG_B)]
    try:
        path = render_matplotlib(hit, OUT_PNG)
        kind = "PNG (matplotlib)"
    except ImportError:
        path = render_svg(hit, OUT_PNG)
        kind = "SVG (matplotlib unavailable)"
    drop = hit[0] / hit[1] if hit[1] > 1e-9 else 0.0
    print(f"[plot_obs2] rendered {kind}  (HITME_HIT {hit[0]:.0f} -> {hit[1]:.0f}, {drop:.1f}x drop)")
    print(f"WROTE {path}")

if __name__ == "__main__":
    main()
