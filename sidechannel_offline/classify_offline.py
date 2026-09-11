#!/usr/bin/env python3
"""Offline ML-model fingerprinting: classify the shipped HitME traces with the
shipped classifiers and render Figure 13 (confusion) and Figure 14 (memorygrams).

    python3 sidechannel_offline/classify_offline.py

Needs only numpy and matplotlib: the classifiers are stored as weight arrays and
run through the forward pass below, not unpickled as sklearn estimators.
"""
import os
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE    = os.path.dirname(os.path.abspath(__file__))
ROOT    = os.path.dirname(HERE)
FIG_DIR = os.path.join(ROOT, "output", "figures")
CUR_DIR = os.path.join(ROOT, "output", "current")
MODEL   = os.path.join(HERE, "models", "fingerprint_mlp.npz")
TRACES  = os.path.join(HERE, "data", "subset_traces.npz")
MEMO_MODELS = ["VGG_16", "VGG_19", "ViT_B_16", "ConvNeXt_Base"]

# ----------------------------------------------------------------------------- #
#  model
# ----------------------------------------------------------------------------- #
def features(X, mode, nbins, nline, ncol):
    """Raw per-set miss counts -> the float32 features the MLPs were fit on."""
    g = np.maximum(X, 0)
    if mode == "all":
        return np.log1p(g.astype(np.float32)).reshape(len(X), -1)
    per = nline // nbins
    g = g[:, :nbins * per * ncol].reshape(-1, nbins, per, ncol)
    m = g.mean(axis=2, dtype=np.float32)
    if mode == "pool_both":
        m = np.concatenate([m, g.max(axis=2).astype(np.float32)], axis=1)
    return np.log1p(m).reshape(len(X), -1)

def forward(S, W, b):
    """3-layer ReLU MLP -> softmax probabilities."""
    A = S
    for i in range(len(W) - 1):
        A = np.maximum(A @ W[i] + b[i], 0.0)
    Z = A @ W[-1] + b[-1]
    Z -= Z.max(axis=1, keepdims=True)
    E = np.exp(Z)
    return E / E.sum(axis=1, keepdims=True)

def load_model():
    M = np.load(MODEL, allow_pickle=False)
    def stack(p):
        n = sum(1 for k in M.files if k.startswith(f"{p}_W"))
        return ([M[f"{p}_W{i}"].astype(np.float32) for i in range(n)],   # fp16 on disk
                [M[f"{p}_b{i}"].astype(np.float32) for i in range(n)])
    return M, stack("arch"), stack("inst")

def scored(proba, y):
    pred = proba.argmax(1)
    k = min(3, proba.shape[1])
    top3 = np.argsort(-proba, axis=1)[:, :k]
    return (pred, float((pred == y).mean()),
            float(np.mean([y[i] in top3[i] for i in range(len(y))])))

# ----------------------------------------------------------------------------- #
#  figures
# ----------------------------------------------------------------------------- #
def plot_confusion(cm, labels, acc, title, out):
    cmn = cm.astype(float) / cm.sum(1, keepdims=True).clip(min=1)
    n = len(labels)
    fig, ax = plt.subplots(figsize=(max(9, n * 0.55), max(8, n * 0.53)))
    im = ax.imshow(cmn, cmap="viridis", vmin=0, vmax=1)
    ax.set_xticks(range(n)); ax.set_yticks(range(n))
    ax.set_xticklabels(labels, rotation=90, fontsize=11)
    ax.set_yticklabels(labels, fontsize=11)
    ax.set_xlabel("Predicted", fontsize=14); ax.set_ylabel("True", fontsize=14)
    ax.set_title(f"DireLeak side channel — {title} detection confusion (shipped traces)\n"
                 f"accuracy = {100*acc:.1f}%  ({n} classes, chance {100/n:.1f}%)", fontsize=15)
    if n <= 16:
        for i in range(n):
            for j in range(n):
                if cmn[i, j] > 0.02:
                    ax.text(j, i, f"{cmn[i,j]:.2f}", ha="center", va="center",
                            color="white" if cmn[i, j] < 0.6 else "black", fontsize=9)
    cb = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    cb.set_label("fraction", fontsize=13); cb.ax.tick_params(labelsize=11)
    fig.tight_layout()
    fig.savefig(os.path.splitext(out)[0] + ".svg")
    fig.savefig(out, dpi=150); plt.close(fig)
    print("WROTE", os.path.relpath(out, ROOT))

def plot_memorygram(X, yi, insts, nline, ncol, out):
    show = [m for m in MEMO_MODELS if m in insts] or [insts[i] for i in sorted(set(yi.tolist()))[:4]]
    fig, axes = plt.subplots(2, len(show), figsize=(3.2 * len(show), 6.4))
    axes = np.atleast_2d(axes)
    for c, name in enumerate(show):
        idx = np.where(yi == insts.index(name))[0]
        for r in range(2):
            g = np.log1p(X[idx[r % len(idx)]].reshape(nline, ncol).astype(np.float32))
            ax = axes[r, c]
            ax.imshow(g, aspect="auto", cmap="magma", origin="lower")
            if r == 0:
                ax.set_title(name, fontsize=11)
            ax.set_xticks([]); ax.set_yticks([])
            if c == 0:
                ax.set_ylabel(f"sample {r+1}\ntime →", fontsize=9)
    fig.suptitle(f"HitME memorygrams — log per-set miss counts "
                 f"(y = {nline} samples = 200 ms,  x = {ncol} HitME sets)", fontsize=11)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(os.path.splitext(out)[0] + ".svg")
    fig.savefig(out, dpi=150); plt.close(fig)
    print("WROTE", os.path.relpath(out, ROOT))

# ----------------------------------------------------------------------------- #
def main():
    os.makedirs(FIG_DIR, exist_ok=True)
    os.makedirs(CUR_DIR, exist_ok=True)
    M, (Wa, ba), (Wi, bi) = load_model()
    d = np.load(TRACES, allow_pickle=False)

    insts = [str(s) for s in M["inst_names"]]
    archs = [str(s) for s in M["arch_names"]]
    nline, ncol = int(M["nline"]), int(M["ncol"])
    mode, nbins = str(M["feat_mode"]), int(M["nbins"])
    X  = d["X"]
    yi = d["y_inst"].astype(int)
    ya = d["y_arch"].astype(int)

    print(f"[offline] models built {str(M['built'])} from real HitME traces "
          f"({nline} samples x {ncol} sets)")
    print(f"[offline] classifying {len(yi)} shipped traces "
          f"({len(set(yi.tolist()))} instances / {len(set(ya.tolist()))} architectures)")

    S = (features(X, mode, nbins, nline, ncol) - M["mean"].astype(np.float32)) \
        / M["scale"].astype(np.float32)
    pa, acc_a, t3_a = scored(forward(S, Wa, ba), ya)
    pi, acc_i, t3_i = scored(forward(S, Wi, bi), yi)

    cell = lambda n: f"{n:2d} classes ({100/n:.1f}%)"
    print("  ┌───────────────┬──────────┬──────────┬────────────────────────┐")
    print("  │ classifier    │ accuracy │  top-3   │ classes (chance)       │")
    print("  ├───────────────┼──────────┼──────────┼────────────────────────┤")
    print(f"  │ architecture  │  {100*acc_a:5.1f}%  │  {100*t3_a:5.1f}%  │ {cell(len(archs)):<22} │")
    print(f"  │ instance      │  {100*acc_i:5.1f}%  │  {100*t3_i:5.1f}%  │ {cell(len(insts)):<22} │")
    print("  └───────────────┴──────────┴──────────┴────────────────────────┘")

    # sc_cm_*.npz feed the orchestrator's inline takeaway (analysis/termshow.py)
    for pred, y, labels, acc, t3, tag, fn, fig in (
            (pa, ya, archs, acc_a, t3_a, "architecture family", "sc_cm_arch.npz", "sc_confusion_arch.png"),
            (pi, yi, insts, acc_i, t3_i, "model instance",      "sc_cm_inst.npz", "sc_confusion.png")):
        cm = np.zeros((len(labels), len(labels)), int)
        for t, p in zip(y, pred):
            cm[t, p] += 1
        np.savez(os.path.join(CUR_DIR, fn), cm=cm, labels=np.array(labels),
                 acc=acc, top3=t3, tag=tag)
        plot_confusion(cm, labels, acc, tag, os.path.join(FIG_DIR, fig))

    plot_memorygram(X, yi, insts, nline, ncol, os.path.join(FIG_DIR, "sc_memorygram.png"))
    print(f"[offline] done — figures in {os.path.relpath(FIG_DIR, ROOT)}/")

if __name__ == "__main__":
    main()
