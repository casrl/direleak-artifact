"""Generate Table 3 (MD cache allocation outcomes, SPR) from data/spr/table3.csv.

Reads the per-transition HITME_HIT residency measurements and re-derives each
cell straight from the data — allocation = (residency > threshold), or "−" when
the transition is not applicable — then prints the paper's Table 3 as a
colour-coded text table (✓ allocate = green, ✗ no = red, – n.a. = dim).

    python3 analysis/gen_table3.py
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THRESHOLD = 0.5

HOSTS = ["L", "RS", "RD"]
INIT  = ["EM", "S"]
ACC   = ["R", "W"]
HOST_LBL = {"L": "L", "RS": "R_S", "RD": "R_D"}
INIT_LBL = {"EM": "E/M", "S": "S"}

# colours
B  = "\x1b[1m"; D = "\x1b[2m"; R = "\x1b[0m"
CY = "\x1b[36m"; GR = "\x1b[32m"; RD = "\x1b[31m"; GY = "\x1b[90m"


def load():
    path = os.path.join(ROOT, "data", "spr", "table3.csv")
    if not os.path.exists(path):
        sys.exit(f"[gen_table3] missing {path}")
    cells = {}
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            key = (r["first_host"], r["init_llc"], r["second_host"], r["access"])
            if int(r["applicable"]) == 0:
                cells[key] = ("–", None)                          # not applicable
            else:
                allocate = float(r["residency"]) > THRESHOLD      # derived from data
                cells[key] = (f"{'✓' if allocate else '✗'}({r['final_llc']})", allocate)
    return cells


def keys():
    for fh in HOSTS:
        for il in INIT:
            for sh in HOSTS:
                for ac in ACC:
                    yield (fh, il, sh, ac)


BW = 19   # visible width of one "First host" block (Init 2nd Acc SPR)


def _spr(txt, alloc):
    col = GY if alloc is None else (GR if alloc else RD)
    return f"{col}{txt:<5}{R}"                      # padded text, colour is zero-width


def _block(cells, fh):
    """One 'First host' block as 16 fixed-width (visible=BW) coloured lines."""
    hdr = f"First host: {HOST_LBL[fh]}"
    lines = [f"{B}{CY}{hdr:<{BW}}{R}",
             f"{D}{'Init':<4} {'2nd':<4} {'Acc':<3} {'SPR':<5}{R}",
             f"{D}{'─' * BW}{R}"]

    def row(il, sh, ac):
        txt, alloc = cells[(fh, il, sh, ac)]
        return f"{INIT_LBL[il]:<4} {HOST_LBL[sh]:<4} {ac:<3} " + _spr(txt, alloc)

    for i, il in enumerate(INIT):
        if i:
            lines.append(f"{D}{'─' * BW}{R}")       # rule between the E/M and S blocks
        for sh in HOSTS:
            for ac in ACC:
                lines.append(row(il, sh, ac))
    return lines


def render_text(cells):
    out = [f"{B}{CY}Table 3 — MD cache allocation outcomes under S_LLC  (SPR){R}",
           f"  {GR}✓{R} allocate    {RD}✗{R} no allocation    {GY}–{R} not applicable"
           f"    (resulting LLC state in parens)", ""]
    blocks = [_block(cells, fh) for fh in HOSTS]
    for parts in zip(*blocks):
        out.append("  " + f"  {D}│{R}  ".join(parts))
    return "\n".join(out)


def summary(cells):
    app = [k for k in keys() if cells[k][1] is not None]
    alloc = [k for k in app if cells[k][1]]
    return len(alloc), len(app)


def main():
    cells = load()
    print(render_text(cells))
    n_alloc, n_app = summary(cells)
    print()
    print(f"[gen_table3] SPR: {B}{GR}{n_alloc}/{n_app}{R} applicable transitions "
          f"allocate an MD entry.")


if __name__ == "__main__":
    main()
