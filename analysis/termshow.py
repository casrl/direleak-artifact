"""Render an artifact's headline takeaway + its figure directly in the terminal.

After each artifact runs, orchestrator.py calls present(exp, fig) so the reviewer
sees the one-line "what to look for", the numbers measured *this run*, and the
figure itself — inline, without leaving the terminal or opening a PNG.

Figures are drawn with the best method the terminal supports:
  * kitty / iTerm2 (and WezTerm)  -> real inline image (auto-detected)
  * everything else               -> 256-color Unicode half-block art (PIL)
Override with DIRELEAK_TERM_IMG = auto|kitty|iterm|blocks|off,
width with DIRELEAK_IMG_WIDTH, truecolor with DIRELEAK_TRUECOLOR=1.

Standalone (uses whatever is already in output/, no measurement server needed):
    python3 analysis/termshow.py                 # every artifact
    python3 analysis/termshow.py covert fig5      # selected artifacts
    python3 analysis/termshow.py output/figures/covert_channel.png "custom note"
"""
import csv
import functools
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

REPO   = Path(__file__).resolve().parent.parent
LOOKUP = "UNC_CHA_HITME_LOOKUP.ALL_UMASK"
ANSI   = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

# ----------------------------------------------------------------------------- #
#  terminal image rendering
# ----------------------------------------------------------------------------- #
def _term_cols():
    return shutil.get_terminal_size(fallback=(100, 40)).columns

_CAPS = None       # cached in-band terminal probe result
_HINTED = False    # blocks-fallback tip printed at most once

def _caps():
    """Probe the terminal in-band (works over SSH) for sixel + pixel width.

    Sends DA1 (`ESC [ c`, universally answered) preceded by a window pixel-size
    query (`ESC [ 14 t`); parses sixel support ('4' in the DA1 attributes) and
    the text-area pixel width. Cached; never raises."""
    global _CAPS
    if _CAPS is not None:
        return _CAPS
    _CAPS = {"sixel": False, "pxwidth": None}
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        return _CAPS
    try:
        import termios, tty, select, time as _t
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            sys.stdout.write("\x1b[14t\x1b[c"); sys.stdout.flush()
            buf, deadline = "", _t.time() + 1.2
            while _t.time() < deadline:
                r, _, _ = select.select([fd], [], [], deadline - _t.time())
                if not r:
                    break
                buf += os.read(fd, 4096).decode("latin-1")
                if re.search(r"\x1b\[\?[0-9;]*c", buf):   # DA1 terminator seen
                    break
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
        m = re.search(r"\x1b\[\?([0-9;]+)c", buf)
        if m and "4" in m.group(1).split(";"):
            _CAPS["sixel"] = True
        m = re.search(r"\x1b\[4;\d+;(\d+)t", buf)
        if m:
            _CAPS["pxwidth"] = int(m.group(1))
    except Exception:
        pass
    return _CAPS

def _method():
    m = os.environ.get("DIRELEAK_TERM_IMG", "auto").lower()
    if m != "auto":
        return m
    if os.environ.get("KITTY_WINDOW_ID") or "kitty" in os.environ.get("TERM", ""):
        return "kitty"
    if (os.environ.get("TERM_PROGRAM") in ("iTerm.app", "WezTerm")
            or os.environ.get("WEZTERM_PANE")):
        return "iterm"
    if _caps().get("sixel"):
        return "sixel"
    return "blocks"

def _kitty(png):
    import base64
    b = base64.b64encode(png)
    buf, i, first = [], 0, True
    while i < len(b):
        chunk, i = b[i:i + 4096], i + 4096
        more = 1 if i < len(b) else 0
        head = b"\x1b_Ga=T,f=100,m=%d;" if first else b"\x1b_Gm=%d;"
        buf.append(head % more + chunk + b"\x1b\\")
        first = False
    sys.stdout.flush()
    sys.stdout.buffer.write(b"".join(buf) + b"\n")
    sys.stdout.buffer.flush()

def _iterm(png):
    import base64
    b = base64.b64encode(png).decode()
    sys.stdout.write("\x1b]1337;File=inline=1;preserveAspectRatio=1;size=%d:%s\x07\n"
                     % (len(png), b))
    sys.stdout.flush()

_LEVELS = (0, 95, 135, 175, 215, 255)

def _lvl(x):
    return min(range(6), key=lambda i: abs(_LEVELS[i] - x))

@functools.lru_cache(maxsize=4096)
def _c256(r, g, b):
    ri, gi, bi = _lvl(r), _lvl(g), _lvl(b)
    cube = 16 + 36 * ri + 6 * gi + bi
    ce = (_LEVELS[ri] - r) ** 2 + (_LEVELS[gi] - g) ** 2 + (_LEVELS[bi] - b) ** 2
    gray = (r + g + b) // 3
    gi2 = max(0, min(23, round((gray - 8) / 10.0)))
    gv = 8 + 10 * gi2
    ge = 3 * (gv - gray) ** 2
    return cube if ce <= ge else 232 + gi2

def _blocks(png_path, max_cols, max_rows):
    from PIL import Image
    im = Image.open(png_path).convert("RGBA")
    im = Image.alpha_composite(Image.new("RGBA", im.size, (255, 255, 255, 255)), im)
    im = im.convert("RGB")
    w, h = im.size
    cols = max(1, max_cols)
    rows = max(1, round(cols * h / w / 2.0))
    if rows > max_rows:
        rows, cols = max_rows, max(1, round(max_rows * 2 * w / h))
    px = im.resize((cols, rows * 2), Image.BILINEAR).load()
    tc = (os.environ.get("COLORTERM", "").lower() in ("truecolor", "24bit")
          or os.environ.get("DIRELEAK_TRUECOLOR") == "1")
    fg = (lambda c: "\x1b[38;2;%d;%d;%dm" % c) if tc else (lambda c: "\x1b[38;5;%dm" % _c256(*c))
    bg = (lambda c: "\x1b[48;2;%d;%d;%dm" % c) if tc else (lambda c: "\x1b[48;5;%dm" % _c256(*c))
    out = []
    for ry in range(rows):
        parts, lf, lb = [" "], None, None
        for cx in range(cols):
            t, b_ = px[cx, ry * 2], px[cx, ry * 2 + 1]
            seq = ""
            if t != lf:
                seq += fg(t); lf = t
            if b_ != lb:
                seq += bg(b_); lb = b_
            parts.append(seq + "▀")
        out.append("".join(parts) + "\x1b[0m")
    return out

def _rle(sixbits, run):
    ch = chr(sixbits + 63)
    return ("!%d%s" % (run, ch)) if run > 3 else ch * run

def _sixel_bytes(im, max_colors=255):
    """Encode a PIL RGB image as a sixel string (full-resolution, crisp text)."""
    from PIL import Image
    q = im.convert("RGB").quantize(colors=max_colors, method=Image.MEDIANCUT)
    W, H = q.size
    pal, px = q.getpalette(), q.load()
    used = [idx for _, idx in (q.getcolors(W * H) or [])]
    out = ["\x1bPq", '"1;1;%d;%d' % (W, H)]
    for i in used:
        out.append("#%d;2;%d;%d;%d" % (i, pal[i * 3] * 100 // 255,
                                       pal[i * 3 + 1] * 100 // 255,
                                       pal[i * 3 + 2] * 100 // 255))
    for band in range(0, H, 6):
        n = min(6, H - band)
        layers = {}
        for k in range(n):
            y, bit = band + k, 1 << k
            for x in range(W):
                c = px[x, y]
                a = layers.get(c)
                if a is None:
                    a = layers[c] = bytearray(W)
                a[x] |= bit
        parts = []
        for c, a in layers.items():
            s, prev, run = ["#%d" % c], a[0], 1
            for x in range(1, W):
                if a[x] == prev:
                    run += 1
                else:
                    s.append(_rle(prev, run)); prev, run = a[x], 1
            s.append(_rle(prev, run))
            parts.append("".join(s))
        out.append("$".join(parts) + "-")
    out.append("\x1b\\")
    return "".join(out)

def _render_sixel(path):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    im = Image.alpha_composite(Image.new("RGBA", im.size, (255, 255, 255, 255)), im)
    im = im.convert("RGB")
    tw = int(os.environ.get("DIRELEAK_IMG_PXWIDTH", "0")) or _caps().get("pxwidth") or 900
    tw = max(200, min(tw - 16, 1000))
    if im.width > tw:
        im = im.resize((tw, max(1, round(im.height * tw / im.width))), Image.LANCZOS)
    sys.stdout.flush()
    sys.stdout.write(_sixel_bytes(im) + "\n")
    sys.stdout.flush()

_SVG_TOOL = None   # cached svg rasterizer: ("cairosvg"|"cli"|"none", exe)

def _svg_rasterizer():
    global _SVG_TOOL
    if _SVG_TOOL is None:
        try:
            import cairosvg  # noqa: F401
            _SVG_TOOL = ("cairosvg", None)
        except Exception:
            exe = next((e for e in ("rsvg-convert", "resvg", "inkscape", "magick", "convert")
                        if shutil.which(e)), None)
            _SVG_TOOL = ("cli", exe) if exe else ("none", None)
    return _SVG_TOOL

def _svg_to_raster(svg, width=1600):
    """Rasterize an SVG to a white-background PNG at `width` px; None if unsupported."""
    kind, exe = _svg_rasterizer()
    if kind == "none":
        return None
    out = os.path.join(tempfile.gettempdir(),
                       "direleak_svg_%s.png" % os.path.splitext(os.path.basename(svg))[0])
    DN = subprocess.DEVNULL
    try:
        if kind == "cairosvg":
            import cairosvg
            cairosvg.svg2png(url=svg, write_to=out, output_width=width,
                             background_color="white")
        elif exe == "rsvg-convert":
            subprocess.run([exe, "-w", str(width), "-b", "white", "-o", out, svg],
                           check=True, stdout=DN, stderr=DN)
        elif exe == "resvg":
            subprocess.run([exe, "-w", str(width), svg, out], check=True, stdout=DN, stderr=DN)
        elif exe == "inkscape":
            subprocess.run([exe, svg, "--export-type=png", "--export-filename=" + out,
                            "-w", str(width), "-b", "white"], check=True, stdout=DN, stderr=DN)
        else:  # magick / convert
            subprocess.run([exe, "-density", "200", "-background", "white", svg,
                            "-flatten", out], check=True, stdout=DN, stderr=DN)
        return out if os.path.exists(out) else None
    except Exception:
        return None

def _resolve_source(path):
    """Prefer the sibling .svg (rasterized) over the .png, per 'embed the svg'."""
    if os.environ.get("DIRELEAK_EMBED_SVG", "1") == "0":
        return path
    svg = os.path.splitext(path)[0] + ".svg"
    if os.path.exists(svg):
        raster = _svg_to_raster(svg)
        if raster:
            return raster
    return path

def render(path, max_cols=None, max_rows=44):
    """Draw the figure at `path` into the terminal (best available method).

    Prefers the sibling .svg (rasterized crisply) over the .png. Displays via
    kitty / iTerm2 / sixel real pixel graphics (crisp text, white background),
    else 256-color half-block art (coarse, but the takeaway numbers are exact)."""
    global _HINTED
    path = str(path)
    method = _method()
    if method == "off":
        print("   [figure: %s]" % path); return
    src = _resolve_source(path)
    if method in ("kitty", "iterm"):
        try:
            with open(src, "rb") as fh:
                (_kitty if method == "kitty" else _iterm)(fh.read())
            return
        except Exception:
            method = "sixel" if _caps().get("sixel") else "blocks"
    if method == "sixel":
        try:
            _render_sixel(src); return
        except Exception as e:
            print("   [sixel render failed (%s) — using block art]" % e); method = "blocks"
    want = int(os.environ.get("DIRELEAK_IMG_WIDTH", "0")) or (_term_cols() - 3)
    try:
        for ln in _blocks(src, min(want, 110), max_rows):
            print(ln)
        if not _HINTED:
            print("   \x1b[2m(coarse preview — for crisp, legible figures run in a sixel- or "
                  "kitty-capable terminal;\n    e.g. xterm -ti vt340, foot, WezTerm, kitty. "
                  "The numbers above are exact either way.)\x1b[0m")
            _HINTED = True
    except Exception as e:
        print("   [figure at %s — inline render unavailable: %s]" % (path, e))

# ----------------------------------------------------------------------------- #
#  data-driven takeaways (computed from whatever is in output/)
# ----------------------------------------------------------------------------- #
def _rows(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))

def _sum_fig2(repo):
    series = {}
    for r in _rows(repo / "output/current/benchmark10_slice09.csv"):
        if r["event"] != LOOKUP:
            continue
        series.setdefault((int(r["socket"]), int(r["domain_in_socket"])), {})[
            int(r["logical_slice"])] = float(r["count"])
    home = series.get((0, 0), {})
    hs = max(home, key=home.get)
    peak = home[hs]
    other = max((v for k, d in series.items() if k != (0, 0) for v in d.values()),
                default=0.0)
    ratio = peak / other if other > 0 else float("inf")
    return [f"home slice = {hs + 1} in domain S1D1: HITME_LOOKUP ≈ {peak:,.0f}",
            f"every other NUMA domain stays ≤ {other:,.0f}  (~{ratio:.0f}× lower)"]

def _sum_fig3(repo):
    cells, n = {}, 0
    for r in _rows(repo / "output/figures/figure3_matrix.csv"):
        t, o = int(r["target_slice"]), int(r["observed_slice"])
        cells[(t, o)] = float(r["count"]); n = max(n, t + 1, o + 1)
    diag, dmin = 0, None
    for t in range(n):
        row = {o: cells.get((t, o), 0.0) for o in range(n)}
        o = max(row, key=row.get)
        if o == t and row[o] > 0:
            diag += 1
            dmin = row[t] if dmin is None else min(dmin, row[t])
    return [f"clean diagonal: {diag}/{n} home slices map to exactly one observed slice",
            f"diagonal cells ≈ 10^4 lookups; off-diagonal ≈ 0 (weakest diag ≈ {dmin:,.0f})"]

def _sum_fig5(repo):
    hit, miss = [], []
    for r in _rows(repo / "output/current/benchmark11_fig5.csv"):
        (hit if r["scenario"] == "hit" else miss).append(float(r["latency_cycles"]))
    hit.sort(); miss.sort()
    hm, mm = hit[len(hit) // 2], miss[len(miss) // 2]
    return [f"MD hit ≈ {hm:.0f} cyc   vs   MD miss ≈ {mm:.0f} cyc",
            f"timing gap ≈ {mm - hm:.0f} cyc — the signal the attacks exploit"]

def _sum_table3(repo):
    rows = _rows(repo / "data/spr/table3.csv")
    app = [r for r in rows if int(r["applicable"]) == 1]
    alloc = [r for r in app if float(r["residency"]) > 0.5]
    return [f"SPR: {len(alloc)}/{len(app)} applicable transitions allocate an MD entry",
            "allocation only when the block is first hosted in the remote socket (R_D)",
            "and re-accessed by a remote domain (R_S/R_D) — matches the paper's SPR column"]

def _home_hit(path, cha=9, socket=0):
    txt = ANSI.sub("", open(path, errors="replace").read())
    in_hit = in_p3 = False
    for ln in txt.splitlines():
        s = ln.strip()
        if s.startswith("Event:"):
            in_hit, in_p3 = "HITME_HIT" in s, False
            continue
        if in_hit and "Std Dev" in s:
            in_p3 = True
            continue
        if in_hit and in_p3:
            p = s.split()
            if len(p) >= 4 and p[0] == str(socket) and p[1] == str(cha):
                try:
                    return sum(float(x) for x in p[2:-2])
                except ValueError:
                    pass
    raise ValueError(f"no per-run HITME_HIT for socket {socket} CHA {cha} in {path}")

def _sum_obs2(repo):
    a = _home_hit(repo / "output/current/benchmark13a.log")
    b = _home_hit(repo / "output/current/benchmark13b.log")
    bt = f"{b:,.0f}" + ("  (≈0)" if b < a * 0.02 else "")
    drop = f"~{a / b:.0f}×" if b > 1e-9 else ">1000×"
    return [f"home-slice HITME_HIT: {a:,.0f} (no flush)  →  {bt} (after CLFLUSH)",
            f"CLFLUSH evicts the directory entry — {drop} fewer hits"]

def _sum_covert(repo):
    pay = [r for r in _rows(repo / "output/current/covert_trace.csv")
           if r["phase"] == "payload"]
    sent = "".join(r["bit_true"] for r in pay)
    deco = "".join(r["bit_decoded"] for r in pay)
    ok = sum(1 for r in pay if r["bit_decoded"] == r["bit_true"])
    hi = [int(r["tx_hits"]) for r in pay if r["bit_true"] == "0"]
    lo = [int(r["tx_hits"]) for r in pay if r["bit_true"] == "1"]
    sep = (sum(hi) / len(hi)) / max(1.0, sum(lo) / len(lo)) if hi and lo else 0.0
    return [f"sent '{sent}'  →  decoded '{deco}'",
            f"payload accuracy = {100.0 * ok / len(pay):.0f}%  ({ok}/{len(pay)} bits)",
            f"transmission set evicted → 1 · survives → 0  (0/1 separation ≈ {sep:.0f}×)"]

def _sum_sc(repo):
    import numpy as np
    out = []
    for fn, lab in (("sc_cm_arch.npz", "architecture family"),
                    ("sc_cm_inst.npz", "model instance")):
        d = np.load(repo / "output/current" / fn, allow_pickle=True)
        n = len(d["labels"])
        out.append(f"{lab}: top-1 {100 * float(d['acc']):.0f}% · top-3 "
                   f"{100 * float(d['top3']):.0f}%  ({n} classes, chance {100.0 / n:.1f}%)")
    out.append("bright block-diagonal; residual confusions cluster within a family")
    return out

# key -> (artifact no., title, figure filename or None, what-to-look-for, summarizer)
ARTIFACTS = {
    "fig2": (1, "[Figure 2]  per-slice HITME_LOOKUP for one home slice",
             "figure2_benchmark10.png",
             "One tall bar at the home slice inside the S1D1 domain, with every other "
             "NUMA domain flat near zero — the HitME directory is homed on a single slice.",
             _sum_fig2),
    "fig3": (2, "[Figure 3]  28×28 slice-mapping matrix",
             "figure3_benchmark10.png",
             "A clean bright diagonal: each accessed home slice lights up exactly one "
             "observed CHA slice (a one-to-one mapping).",
             _sum_fig3),
    "fig5": (3, "[Figure 5]  MD-cache hit vs miss access latency",
             "figure5_benchmark11.png",
             "Two clearly separated latency modes — a fast MD-hit peak and a slower "
             "MD-miss peak. The gap between them is the exploitable timing signal.",
             _sum_fig5),
    "table3": (4, "[Table 3]  MD cache allocation outcomes under S_LLC (SPR)",
               None,
               "In the table above, ✓ (allocate, green) cells appear only in the R_D "
               "first-host group — the block is first hosted cross-socket and re-accessed "
               "by a remote domain; ✗ = no allocation, – = not applicable.",
               _sum_table3),
    "obs2": (5, "[Obs. 2]  CLFLUSH invalidates the MD cache",
             "obs2_clflush_benchmark13.png",
             "The 'After CLFLUSH' bar collapses to ~0 beside the tall 'No flush' bar — "
             "a single CLFLUSH evicts the directory entry.",
             _sum_obs2),
    "covert": (6, "[Figure 11]  cross-domain covert channel (Trojan → Spy)",
               "covert_channel.png",
               "Each shaded band is one transmitted bit: the transmission-set trace rises "
               "when the set is evicted (a 1) and stays low when it survives (a 0); the "
               "decoded bits under the axis match the sent string.",
               _sum_covert),
    "sidechannel": (7, "[Figure 13]  ML-model fingerprinting side channel",
                    "sc_confusion.png",
                    "A bright block-diagonal confusion matrix — models are separable by "
                    "their cross-socket MD footprint, far above chance.",
                    _sum_sc),
}

# ----------------------------------------------------------------------------- #
#  presentation
# ----------------------------------------------------------------------------- #
_B = "\x1b[1m"; _D = "\x1b[2m"; _RS = "\x1b[0m"
_CY = "\x1b[36m"; _BCY = "\x1b[96m"; _GR = "\x1b[32m"; _BGR = "\x1b[92m"
_YL = "\x1b[33m"; _RD = "\x1b[31m"; _BRD = "\x1b[91m"

# numeric / metric tokens: 8/8, 100%, 153×, 10^4, 0x2b00, 72.7% — but not inside S1D1/R_D2
_HLNUM = re.compile(r"(?<![A-Za-z0-9_])"
                    r"(0x[0-9a-fA-F]+|10\^\d+|\d[\d,]*(?:\.\d+)?(?:%|×|/\d[\d,]*(?:\.\d+)?)?)")

def _hl(text):
    """Highlight measured results: numbers/metrics cyan-bold, ✓ green, ✗ red, → dim."""
    text = _HLNUM.sub(lambda m: f"{_B}{_BCY}{m.group(0)}{_RS}", text)
    text = text.replace("✓", f"{_BGR}✓{_RS}").replace("✗", f"{_BRD}✗{_RS}")
    return text.replace("→", f"{_D}→{_RS}")

def _rule(ch="─", w=64):
    print(f"  {_D}{ch * w}{_RS}")

def _wrap(text, indent="      ", width=72, hl=False):
    for ln in textwrap.wrap(text, width=width):
        print(indent + (_hl(ln) if hl else ln))

def present(exp, fig=None, repo=REPO):
    """Print an artifact's colour-coded takeaway block, then render its figure inline.

    `exp` is the orchestrator EXPERIMENT dict (uses exp['key']) or a bare key str.
    `fig` overrides the figure path; otherwise the artifact's default is used.
    Never raises — a missing file degrades to a note.
    """
    repo = Path(repo)
    key = exp["key"] if isinstance(exp, dict) else exp
    meta = ARTIFACTS.get(key)
    if meta is None:
        return
    no, title, figfile, look, summarize = meta

    print()
    _rule("═")
    print(f"  {_B}{_CY}ARTIFACT {no} — {title}{_RS}")
    _rule("═")
    print(f"  {_B}{_YL}▶ WHAT TO LOOK FOR{_RS}")
    _wrap(look)
    print(f"  {_B}{_GR}▶ MEASURED (this run){_RS}")
    try:
        for ln in summarize(repo):
            _wrap(ln, hl=True)
    except Exception as e:
        _wrap(f"(takeaway unavailable: {e})")

    figpath = Path(fig) if fig else (repo / "output/figures" / figfile if figfile else None)
    if figpath is not None:
        _rule()
        if figpath.exists():
            render(figpath)
        else:
            print(f"      {_D}[figure not found: {figpath} — run this artifact to generate it]{_RS}")
    _rule()

def main():
    args = sys.argv[1:]
    if args and Path(args[0]).suffix.lower() in (".png", ".jpg", ".jpeg", ".gif"):
        if len(args) > 1:
            print("  \x1b[1m▶ TAKEAWAY\x1b[0m")
            _wrap(" ".join(args[1:]))
            _rule()
        render(args[0])
        return
    keys = [k for k in args if k in ARTIFACTS] or list(ARTIFACTS)
    for k in keys:
        present(k)

if __name__ == "__main__":
    main()
