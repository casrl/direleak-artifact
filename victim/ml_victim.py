import os
import sys
import signal
import time

NODE_CPUS = {0: range(0, 24), 1: range(24, 48), 2: range(48, 72), 3: range(72, 96)}
VICTIM_NODES = (1, 3)

def pin_cross_socket(per_socket):
    cpus = []
    for node in VICTIM_NODES:
        cpus += list(NODE_CPUS[node])[:per_socket]
    os.sched_setaffinity(0, set(cpus))
    return cpus

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: ml_victim.py <model_name> [threads_per_socket=8]")
    name = sys.argv[1]
    per_socket = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    cpus = pin_cross_socket(per_socket)
    os.environ["MKL_THREADING_LAYER"] = "GNU"
    os.environ.setdefault("OMP_NUM_THREADS", str(len(cpus)))
    os.environ.setdefault("OMP_PROC_BIND", "spread")

    import torch
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import models as M

    torch.set_num_threads(len(cpus))
    torch.set_grad_enabled(False)

    model, insz = M.build(name)
    x = torch.randn(1, 3, insz, insz)

    run = {"go": True}
    signal.signal(signal.SIGTERM, lambda *a: run.__setitem__("go", False))
    signal.signal(signal.SIGINT,  lambda *a: run.__setitem__("go", False))

    for _ in range(2):
        model(x)
    sys.stderr.write(f"[victim] {name} running on cpus {cpus[0]}..{cpus[-1]} "
                     f"({len(cpus)} threads, input {insz}) — inference loop\n")
    sys.stderr.flush()

    n = 0
    t0 = time.time()
    while run["go"]:
        model(x)
        n += 1
    dt = time.time() - t0
    sys.stderr.write(f"[victim] {name} stopped: {n} inferences in {dt:.1f}s "
                     f"({n/max(dt,1e-9):.1f}/s)\n")

if __name__ == "__main__":
    main()
