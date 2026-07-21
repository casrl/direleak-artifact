import os, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def plot_cm(path, out):
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    d = np.load(path, allow_pickle=True)
    cm = d["cm"].astype(float); labels = list(d["labels"]); acc = float(d["acc"]); tag = str(d["tag"])
    cmn = cm / cm.sum(1, keepdims=True).clip(min=1)
    n = len(labels)
    fig, ax = plt.subplots(figsize=(max(9, n*0.55), max(8, n*0.53)))
    im = ax.imshow(cmn, cmap="viridis", vmin=0, vmax=1)
    ax.set_xticks(range(n)); ax.set_yticks(range(n))
    ax.set_xticklabels(labels, rotation=90, fontsize=11)
    ax.set_yticklabels(labels, fontsize=11)
    ax.set_xlabel("Predicted", fontsize=14); ax.set_ylabel("True", fontsize=14)
    ax.set_title(f"DireLeak side channel — {tag} detection confusion (held-out run)\n"
                 f"accuracy = {100*acc:.1f}%  ({n} classes, chance {100/n:.1f}%)", fontsize=15)
    if n <= 16:
        for i in range(n):
            for j in range(n):
                if cmn[i, j] > 0.02:
                    ax.text(j, i, f"{cmn[i,j]:.2f}", ha="center", va="center",
                            color="white" if cmn[i, j] < 0.6 else "black", fontsize=9)
    cb = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04, label="fraction")
    cb.set_label("fraction", fontsize=13); cb.ax.tick_params(labelsize=11)
    fig.tight_layout()
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    fig.savefig(os.path.splitext(out)[0] + ".svg")
    fig.savefig(out, dpi=150); print("WROTE", out)

def plot_gram(path, out):
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    d = np.load(path, allow_pickle=True)
    X = d["X"]; yi = d["y_inst"]; run = d["run"]
    nsamp, nf = int(d["nsamp"]), int(d["nf"])
    names = list(d["inst_names"])
    show = ["vgg19", "resnet152", "densenet201", "squeezenet1_1"]
    show = [s for s in show if s in names] or [names[i] for i in sorted(set(yi.tolist()))[:4]]
    fig, axes = plt.subplots(2, len(show), figsize=(3.0*len(show), 6))
    axes = np.atleast_2d(axes)
    for c, name in enumerate(show):
        iid = names.index(name)
        idx = np.where(yi == iid)[0]
        for r in range(2):
            g = np.log1p(X[idx[r]].reshape(nsamp, nf))
            ax = axes[r, c]
            ax.imshow(g, aspect="auto", cmap="magma", origin="lower")
            if r == 0: ax.set_title(name, fontsize=10)
            ax.set_xticks([]);
            ax.set_ylabel(f"sample {r+1}\ntime →" if c == 0 else "", fontsize=8)
            ax.set_yticks([])
    fig.suptitle("HitME memorygrams (log HITME_HIT/LOOKUP per CHA over 40 samples = 200ms)\n"
                 "x = 224 counters (2 sockets × 56 CHA × {hit,look}),  y = time",
                 fontsize=10)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    fig.savefig(os.path.splitext(out)[0] + ".svg")
    fig.savefig(out, dpi=150); print("WROTE", out)

def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "cm"
    if what == "cm":
        p = sys.argv[2] if len(sys.argv) > 2 else "output/current/sc_cm_inst.npz"
        plot_cm(os.path.join(ROOT, p), os.path.join(ROOT, "output/figures/sc_confusion.png"))
    elif what == "gram":
        p = sys.argv[2] if len(sys.argv) > 2 else "output/current/sidechannel.npz"
        plot_gram(os.path.join(ROOT, p), os.path.join(ROOT, "output/figures/sc_memorygram.png"))
    else:
        sys.exit("usage: plot_sidechannel.py cm|gram [npz]")

if __name__ == "__main__":
    main()
