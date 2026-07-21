import argparse, os, sys
os.environ.setdefault("MKL_THREADING_LAYER", "GNU")
import numpy as np
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def split_by_run(run, test_runs):
    runs = sorted(set(run.tolist()))
    test = set(runs[-test_runs:]) if 0 < test_runs < len(runs) else {runs[-1]}
    te = np.array([r in test for r in run])
    return ~te, te

def run_clf(Xtr, ytr, Xte, yte, names, epochs, tag, cm_out=None):
    sc = StandardScaler().fit(Xtr)
    Xtr, Xte = sc.transform(Xtr), sc.transform(Xte)
    clf = MLPClassifier(hidden_layer_sizes=(512, 256), activation="relu",
                        alpha=1e-3, batch_size=64, max_iter=epochs,
                        early_stopping=True, n_iter_no_change=20,
                        validation_fraction=0.15, random_state=0)
    clf.fit(Xtr, ytr)
    proba = clf.predict_proba(Xte)
    classes = clf.classes_
    pred = classes[proba.argmax(1)]
    k = min(3, len(classes))
    top3 = classes[np.argsort(-proba, 1)[:, :k]]
    acc = (pred == yte).mean()
    t3 = np.mean([yte[i] in top3[i] for i in range(len(yte))])
    nclass = len(set(ytr.tolist()) | set(yte.tolist()))
    print(f"  [{tag:8s}] test acc = {100*acc:5.1f}%   top-3 = {100*t3:5.1f}%   "
          f"(train {len(Xtr)}, test {len(Xte)}, {nclass} classes, chance {100/nclass:.1f}%)")
    if cm_out is not None:
        labs = sorted(set(yte.tolist()) | set(ytr.tolist()))
        cm = np.zeros((len(labs), len(labs)), int)
        idx = {c: i for i, c in enumerate(labs)}
        for t, p in zip(yte, pred):
            cm[idx[t], idx[p]] += 1
        np.savez(cm_out, cm=cm, labels=np.array([names[c] for c in labs]),
                 acc=acc, top3=t3, tag=tag)
    return acc, t3

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("npz", nargs="?", default="output/current/sidechannel.npz")
    ap.add_argument("--test_runs", type=int, default=1)
    ap.add_argument("--epochs", type=int, default=300)
    ap.add_argument("--cm_prefix", default="output/current/sc_cm")
    a = ap.parse_args()

    d = np.load(os.path.join(ROOT, a.npz), allow_pickle=True)
    X, yi, ya, run = d["X"], d["y_inst"], d["y_arch"], d["run"]
    nsamp, nf = int(d["nsamp"]), int(d["nf"])
    inst_names = list(d["inst_names"]); arch_names = list(d["arch_names"])
    print(f"[train] {X.shape[0]} fingerprints, {X.shape[1]} feats ({nsamp}x{nf}), "
          f"{len(set(yi.tolist()))} instances, {len(set(ya.tolist()))} archs, "
          f"runs={sorted(set(run.tolist()))}")

    Xf = np.log1p(np.maximum(X, 0)).astype(np.float32)
    tr, te = split_by_run(run, a.test_runs)
    print(f"[train] split by run: {tr.sum()} train / {te.sum()} test "
          f"(held-out run(s) = {sorted(set(run[te].tolist()))})")

    run_clf(Xf[tr], ya[tr], Xf[te], ya[te], arch_names, a.epochs, "ARCH",
            os.path.join(ROOT, a.cm_prefix + "_arch.npz"))
    run_clf(Xf[tr], yi[tr], Xf[te], yi[te], inst_names, a.epochs, "INSTANCE",
            os.path.join(ROOT, a.cm_prefix + "_inst.npz"))

if __name__ == "__main__":
    main()
