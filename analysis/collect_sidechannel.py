import argparse, os, subprocess, sys, tempfile, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "victim"))
import models as M

NF = 2 * 56 * 2

def sh(cmd):
    return subprocess.run(cmd, shell=True, cwd=ROOT)

def ensure_writable(d):
    os.makedirs(d, exist_ok=True)
    if not os.access(d, os.W_OK):
        u = os.environ.get("SUDO_USER") or os.environ.get("USER") or ""
        if u:
            subprocess.run(f"sudo -n chown -R {u}:{u} {d}", shell=True)
    return os.access(d, os.W_OK)

def collect_run(model, inst_id, fp, nsamp, interval, warmup, mon_cpu):
    vlog = open(f"/tmp/vic_{model}.log", "w")

    vic = subprocess.Popen(["numactl", "--interleave=0,1,2,3",
                            "python3", "victim/ml_victim.py", model, "8"],
                           cwd=ROOT, stdout=vlog, stderr=subprocess.STDOUT)
    try:
        time.sleep(warmup)
        if vic.poll() is not None:
            vlog.flush()
            err = open(f"/tmp/vic_{model}.log").read()[-400:]
            sys.stderr.write(f"  [!] victim {model} died during warmup (rc={vic.returncode}): {err}\n")
            return None
        tmp = tempfile.mktemp(suffix=".csv")
        r = sh(f"sudo -n taskset -c {mon_cpu} ./tools/hitme_monitor {tmp} {inst_id} {fp} {nsamp} {interval}")
        if r.returncode != 0 or not os.path.exists(tmp):
            sys.stderr.write(f"  [!] monitor failed for {model}\n"); return None
        d = np.loadtxt(tmp, delimiter=",")
        subprocess.run(f"sudo -n rm -f {tmp}", shell=True)
        if d.ndim == 1:
            d = d[None, :]
        return d[:, 2:]
    finally:
        vic.terminate()
        try: vic.wait(timeout=10)
        except Exception: vic.kill()
        time.sleep(0.5)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="ALL")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--fp", type=int, default=20)
    ap.add_argument("--nsamp", type=int, default=40)
    ap.add_argument("--interval", type=int, default=5000)
    ap.add_argument("--warmup", type=float, default=3.0)
    ap.add_argument("--mon_cpu", type=int, default=60)
    ap.add_argument("--out", default="output/current/sidechannel.npz")
    a = ap.parse_args()

    out = os.path.join(ROOT, a.out)
    if not ensure_writable(os.path.dirname(out) or ROOT):
        sys.exit(f"[collect] output dir not writable: {os.path.dirname(out)} — chown it to your user and retry")

    names = M.INSTANCE_NAMES if a.models == "ALL" else a.models.split(",")
    X, yi, ya, run = [], [], [], []
    t0 = time.time()
    for mi, name in enumerate(names):
        inst_id = M.INSTANCE_ID[name]
        arch_id = M.ARCH_ID[M.INSTANCE_ARCH[name]]
        got = 0
        for r in range(a.runs):
            f = collect_run(name, inst_id, a.fp, a.nsamp, a.interval, a.warmup, a.mon_cpu)
            if f is None:
                continue
            X.append(f); yi += [inst_id]*len(f); ya += [arch_id]*len(f); run += [r]*len(f)
            got += len(f)
        el = time.time()-t0
        sys.stderr.write(f"[{mi+1}/{len(names)}] {name:20s} inst={inst_id} arch={M.INSTANCE_ARCH[name]:11s} "
                         f"-> {got} fps  ({el:.0f}s elapsed)\n"); sys.stderr.flush()

    if not X:
        sys.exit("[collect] no fingerprints collected (every run failed) — nothing to save")
    X = np.vstack(X).astype(np.float32)
    yi = np.array(yi); ya = np.array(ya); run = np.array(run)
    payload = dict(X=X, y_inst=yi, y_arch=ya, run=run, nsamp=a.nsamp, nf=NF,
                   inst_names=np.array(M.INSTANCE_NAMES), arch_names=np.array(M.ARCH_CLASSES))
    try:
        np.savez_compressed(out, **payload)
    except Exception as e:
        rescue = os.path.join(tempfile.gettempdir(), "sidechannel_rescue.npz")
        np.savez_compressed(rescue, **payload)
        sys.exit(f"[collect] FAILED to write {a.out} ({e}); {len(X)} fingerprints rescued -> {rescue} "
                 f"(move it into place: mv {rescue} {out})")
    print(f"[collect] wrote {a.out}: X={X.shape}, {len(set(yi.tolist()))} instances, "
          f"{len(set(ya.tolist()))} archs, {a.runs} runs, {len(X)} fingerprints")

if __name__ == "__main__":
    main()
