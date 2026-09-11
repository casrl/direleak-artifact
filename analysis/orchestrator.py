import csv
import functools
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

import termshow  # inline terminal figure + data-driven takeaway (analysis/termshow.py)

print = functools.partial(print, flush=True)

REPO       = Path(__file__).resolve().parent.parent
SESSION    = "direleak"
SERVER_LOG = REPO / "output" / "server.log"
FIG_DIR    = REPO / "output" / "figures"
SLICES_PER_DOMAIN = 28
MSR_CMD    = os.environ.get("DIRELEAK_MSR_CMD", "sudo ./bin/msr_program --server --no-evset")
ARCH       = os.environ.get("DIRELEAK_ARCH", "spr")

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
LOOKUP = "UNC_CHA_HITME_LOOKUP.ALL_UMASK"

EXPERIMENTS = [
    {"key": "fig2", "kind": "single",
     "title": "[Figure. 2] per-slice HITME_LOOKUP for one home slice",
     "benchmark": "benchmark10", "slice": 9,
     "plotter": "analysis/plot_figure2.py", "figure": "figure2_benchmark10.png"},
    {"key": "fig3", "kind": "sweep",
     "title": f"[Figure. 3] {SLICES_PER_DOMAIN}x{SLICES_PER_DOMAIN} slice-mapping matrix",
     "benchmark": "benchmark10", "slices": SLICES_PER_DOMAIN,
     "plotter": "analysis/plot_figure3.py", "figure": "figure3_benchmark10.png"},
    {"key": "fig5", "kind": "single",
     "title": "[Figure. 5] MD-cache hit vs miss access latency",
     "benchmark": "benchmark11", "slice": 9, "csv": "output/current/benchmark11_fig5.csv",
     "plotter": "analysis/plot_figure5.py", "figure": "figure5_benchmark11.png"},
    {"key": "table3", "kind": "datatable",
     "title": "[Table. 3] MD cache allocation outcomes under S_LLC (SPR)",
     "generator": "analysis/gen_table3.py"},
    {"key": "obs2", "kind": "pair",
     "title": "[Obs. 2] CLFLUSH invalidates the MD cache (flush vs no-flush)",
     "benchmarks": ["benchmark13a", "benchmark13b"],
     "logs": ["output/current/benchmark13a.log", "output/current/benchmark13b.log"],
     "plotter": "analysis/plot_obs2.py", "figure": "obs2_clflush_benchmark13.png"},
    {"key": "covert", "kind": "covert",
     "title": "[Figure. 11] cross-domain covert channel (Trojan -> Spy)",
     "script": "tools/run_covert.sh", "args": ["2", "8", "01101001", "16"],
     "plotter": "analysis/plot_covert.py", "figure": "covert_channel.png"},
    {"key": "sidechannel", "kind": "sidechannel",
     "title": "[Figure. 13+14] ML-model fingerprinting side channel (30 models, offline)",
     "script": "sidechannel_offline/classify_offline.py"},
]

def slice_csv(cha):
    return REPO / "output" / "current" / f"benchmark10_slice{cha:02d}.csv"

def tmux(*a):
    return subprocess.run(["tmux", *a], capture_output=True, text=True)

def have_tmux():
    return subprocess.run(["which", "tmux"], capture_output=True).returncode == 0

def session_exists():
    return tmux("has-session", "-t", SESSION).returncode == 0

def start_session():
    (REPO / "output").mkdir(parents=True, exist_ok=True)
    SERVER_LOG.write_text("")
    inner = (f"cd {shlex.quote(str(REPO))} && {MSR_CMD} 2>&1 | "
             f"tee {shlex.quote(str(SERVER_LOG))}; echo __SERVER_EXITED__; exec bash")
    tmux("new-session", "-d", "-s", SESSION, "-x", "220", "-y", "50", inner)

def kill_session():
    tmux("kill-session", "-t", SESSION)

def send_line(text):
    tmux("send-keys", "-t", SESSION, "-l", text)
    tmux("send-keys", "-t", SESSION, "Enter")

def log_size():
    return SERVER_LOG.stat().st_size if SERVER_LOG.exists() else 0

SPIN = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"

def phase_hint(s):
    if not s:
        return "starting measurement server"
    if "%" in s:
        c = re.sub(r"\[[^\]]*\]", "", s)
        c = re.sub(r"\s{2,}", " ", c).strip(" |")
        return c if len(c) <= 66 else c[:63] + "…"
    t = s.lower()
    if "gen_cha" in t or "offset=" in t or "cha mapping" in t:
        return "building per-CHA address lists (one-time, a few minutes)"
    if "memory allocation" in t:
        return "allocating hugepage buffer pool"
    if "max_sockets" in t or "numa" in t or "primary core" in t or "find_cha" in t:
        return "detecting socket / CHA topology"
    if "evset" in t or "eviction" in t or "l2demote" in t or "building cha-0" in t:
        return "building eviction sets"
    return s if len(s) <= 66 else s[:63] + "…"

def tail_wait(markers, start_offset=0, timeout=1800, stream=True, hint_after=15):
    deadline = time.time() + timeout
    is_tty = sys.stdout.isatty()
    t0 = time.time()
    last_out = t0
    last_draw = 0.0
    hinted = False
    si = 0
    spinning = False
    status = "starting measurement server"

    def clear_spin():
        nonlocal spinning
        if spinning:
            sys.stdout.write("\r\x1b[K"); sys.stdout.flush()
            spinning = False

    def draw():
        nonlocal spinning, si, last_draw
        now = time.time()
        if now - last_draw < 0.1:
            return
        last_draw = now
        el = int(now - t0)
        sys.stdout.write(f"\r   \x1b[31m{SPIN[si % len(SPIN)]}\x1b[0m Please wait — {status}"
                         f"   [{el//60}m{el%60:02d}s]\x1b[K")
        sys.stdout.flush()
        spinning = True; si += 1

    while not SERVER_LOG.exists() and time.time() < deadline:
        time.sleep(0.2)
    if not SERVER_LOG.exists():
        return None
    with open(SERVER_LOG, "rb") as fh:
        fh.seek(start_offset)
        buf = ""
        while time.time() < deadline:
            chunk = fh.read()
            if chunk:
                last_out = time.time()
                buf += chunk.decode(errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    raw = ANSI.sub("", line)
                    disp = raw.split("\r")[-1].strip()
                    flat = raw.replace("\r", "")
                    if disp:
                        status = phase_hint(disp)
                    if stream and disp:
                        clear_spin()
                        print("   " + disp)
                    if "__SERVER_EXITED__" in flat:
                        clear_spin(); return "__SERVER_EXITED__"
                    for m in markers:
                        if m in flat:
                            clear_spin(); return m
                tail = ANSI.sub("", buf).split("\r")[-1].strip()
                if tail:
                    status = phase_hint(tail)
                if "\n" not in buf and "\r" in buf:
                    buf = buf.rsplit("\r", 1)[-1]
                if stream and is_tty:
                    draw()
            elif stream and is_tty:
                draw()
                time.sleep(0.1)
            else:
                if stream and not is_tty and not hinted and time.time() - t0 > hint_after:
                    hinted = True
                    print(f"   … still working ({status}; attach: tmux attach -t {SESSION}).")
                time.sleep(0.3)
    clear_spin()
    return None

def server_ready():
    if not session_exists() or not SERVER_LOG.exists():
        return False
    txt = SERVER_LOG.read_text(errors="replace")
    return "__SERVER_READY__" in txt and "__SERVER_EXITED__" not in txt

def server_status():
    if not session_exists():
        return "not running"
    if server_ready():
        return "ready"
    if SERVER_LOG.exists() and "__SERVER_EXITED__" in SERVER_LOG.read_text(errors="replace"):
        return "exited (needs restart)"
    return "starting…"

def run_setup_env():
    print("• setup_env.sh (system + python deps, msr module, CPU pinning) …")
    # Pass our interpreter so setup installs the Python libs into the exact
    # python3 that runs the analysis (via `env` so it survives sudo's env reset).
    cmd = ["sudo", "-n", "env", f"SETUP_PYTHON={sys.executable}", "./setup_env.sh"]
    if stream(cmd) != 0:
        print("  setup_env.sh returned nonzero — continuing (environment may already be configured).")

def build_all():
    print(f"• building (make ARCH={ARCH}, covert, sidechannel) …")
    if stream(["make", f"ARCH={ARCH}"]) != 0:
        print("✗ build failed (make ARCH=%s)." % ARCH); return False
    if stream(["make", "covert", "sidechannel"]) != 0:
        print("✗ tool build failed (make covert sidechannel)."); return False
    print("✓ build complete.")
    return True

SERVER_WAS_UP = False

def ensure_server():
    global SERVER_WAS_UP
    run_setup_env()
    if session_exists():
        if server_ready():
            return True
        print(f"• server exists but isn't ready — waiting for setup…")
    else:
        verb = "restarting" if SERVER_WAS_UP else "launching"
        print(f"• {verb} server in tmux '{SESSION}' (one-time address gen — a while)…")
        if not build_all():
            return False
        start_session()
    got = tail_wait(["__SERVER_READY__"], start_offset=0, timeout=3600)
    if got == "__SERVER_READY__":
        print("✓ server ready.")
        SERVER_WAS_UP = True
        return True
    print(f"✗ server did not become ready (attach: tmux attach -t {SESSION}).")
    return False

def stop_server():
    if session_exists():
        try:
            send_line("quit"); time.sleep(1)
        except Exception:
            pass
        kill_session()
    subprocess.run(["sudo", "pkill", "-9", "-x", "msr_program"], capture_output=True)
    time.sleep(1)

def run_benchmark(benchmark, arg=None):
    off = log_size()
    cmd = benchmark if arg is None else f"{benchmark} {arg}"
    send_line(cmd)
    got = tail_wait([f"__BENCH_DONE__ {cmd}", f"__BENCH_ERR__ {cmd}"], start_offset=off, timeout=1800)
    return (got == f"__BENCH_DONE__ {cmd}"), got, off

def stream(cmd, **kw):
    import select
    p = subprocess.Popen(cmd, cwd=str(REPO), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1, **kw)
    is_tty = sys.stdout.isatty()
    fd = p.stdout.fileno()
    t0 = time.time(); si = 0; spinning = False; last = ""

    def clear():
        nonlocal spinning
        if spinning:
            sys.stdout.write("\r\x1b[K"); sys.stdout.flush(); spinning = False

    while True:
        r, _, _ = select.select([fd], [], [], 0.12)
        if r:
            ln = p.stdout.readline()
            if ln == "":
                break
            s = ln.rstrip()
            if s:
                clear()
                print("   " + s)
                last = ANSI.sub("", s).strip()
        elif p.poll() is not None:
            break
        elif is_tty and last:
            el = int(time.time() - t0)
            ctx = last if len(last) <= 78 else last[:75] + "…"
            sys.stdout.write(f"\r   \x1b[31m{SPIN[si % len(SPIN)]}\x1b[0m {ctx}"
                             f"   [{el//60}m{el%60:02d}s]\x1b[K")
            sys.stdout.flush(); spinning = True; si += 1
    clear()
    for ln in p.stdout:
        s = ln.rstrip()
        if s:
            print("   " + s)
    p.wait()
    return p.returncode

def run_plotter(plotter, args, figure):
    FIG_DIR.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run([sys.executable, str(REPO / plotter), *[str(a) for a in args],
                           str(FIG_DIR / figure)] if figure else
                          [sys.executable, str(REPO / plotter), *[str(a) for a in args]],
                          capture_output=True, text=True, cwd=str(REPO))
    for ln in (proc.stdout + proc.stderr).strip().splitlines():
        print("   " + ln)
    m = re.search(r"WROTE (.+)$", proc.stdout, re.M)
    if m:
        print(f"✓ figure: {m.group(1).strip()}")

def present(exp, fig=None):
    """Show this artifact's data-driven takeaway + its figure inline in the terminal."""
    try:
        termshow.present(exp, fig)
    except Exception as e:
        print(f"   (inline takeaway/figure unavailable: {e})")

def s1d1_lookup_row(csv_path):
    row = {}
    with open(csv_path, newline="") as fh:
        for r in csv.DictReader(fh):
            if r["event"] == LOOKUP and int(r["socket"]) == 0 and int(r["domain_in_socket"]) == 0:
                row[int(r["logical_slice"])] = float(r["count"])
    return row

def run_single(exp):
    if not ensure_server():
        return
    cha = exp["slice"]
    csv_path = (REPO / exp["csv"]) if "csv" in exp else slice_csv(cha)
    print(f"▶ {exp['title']}\n  running '{exp['benchmark']} {cha}' …")
    ok, got, _ = run_benchmark(exp["benchmark"], cha)
    if ok:
        run_plotter(exp["plotter"], [csv_path], exp["figure"])
        present(exp, FIG_DIR / exp["figure"])
    else:
        print(f"✗ '{exp['benchmark']} {cha}' did not complete ({got}).")

def run_sweep(exp):
    if not ensure_server():
        return
    n = exp["slices"]
    print(f"▶ {exp['title']}\n  sweeping slices 0-{n-1} …")
    rows = {}
    for cha in range(n):
        print(f"  [{cha+1:2d}/{n}] slice {cha}")
        ok, got, _ = run_benchmark(exp["benchmark"], cha)
        if got == "__SERVER_EXITED__":
            print("✗ server exited mid-sweep."); return
        rows[cha] = s1d1_lookup_row(slice_csv(cha)) if ok and slice_csv(cha).exists() else {}
    matrix_csv = FIG_DIR / "figure3_matrix.csv"
    FIG_DIR.mkdir(parents=True, exist_ok=True)
    with open(matrix_csv, "w", newline="") as fh:
        w = csv.writer(fh); w.writerow(["target_slice", "observed_slice", "count"])
        for t in range(n):
            for o in range(n):
                w.writerow([t, o, f"{rows.get(t, {}).get(o, 0.0):.2f}"])
    run_plotter(exp["plotter"], [matrix_csv], exp["figure"])
    present(exp, FIG_DIR / exp["figure"])

def run_datatable(exp):
    # Table 3 is reconstructed from the reference data in data/<gen>/table3.csv;
    # no measurement server or benchmark run is required.
    print(f"▶ {exp['title']}\n  generating table from data/ …")
    if stream([sys.executable, exp["generator"]]) == 0:
        present(exp)
    else:
        print("✗ table generation failed.")

def run_pair(exp):
    if not ensure_server():
        return
    print(f"▶ {exp['title']}")
    for bm in exp["benchmarks"]:
        print(f"  running '{bm}' …")
        ok, got, _ = run_benchmark(bm)
        if not ok:
            print(f"✗ '{bm}' did not complete ({got})."); return
    logs = [REPO / p for p in exp["logs"]]
    if any(not p.exists() for p in logs):
        print("✗ expected counter log(s) missing."); return
    run_plotter(exp["plotter"], logs, exp["figure"])
    present(exp, FIG_DIR / exp["figure"])

def ensure_tools(*names):
    if all((REPO / "tools" / n).exists() for n in names):
        return True
    run_setup_env()
    return build_all()

def run_covert(exp):
    print(f"▶ {exp['title']}\n  stopping server; running Trojan + Spy (~4 min) …")
    if not ensure_tools("spy", "trojan"):
        return
    stop_server()
    stream(["sudo", "-n", "./" + exp["script"], *exp["args"]])
    trace = REPO / "output" / "current" / "covert_trace.csv"
    if trace.exists():
        run_plotter(exp["plotter"], [str(trace)], exp["figure"])
        present(exp, FIG_DIR / exp["figure"])
    else:
        print("✗ covert run produced no trace (see errors above).")
    print("  NOTE: server was stopped for this artifact; it will auto-restart when you pick a framework artifact (or press 'r').")

def run_sidechannel(exp):
    # replayed from the shipped classifiers + trace subset: no hardware, no root,
    # and the counter server keeps running.
    print(f"▶ {exp['title']}")
    print("  classifying the shipped trace subset with the pre-trained models (a few seconds) …")
    rc = stream(["python3", exp["script"]])
    if rc != 0:
        print("✗ offline classification failed."); return
    present(exp, FIG_DIR / "sc_confusion.png")
    present("sc_gram")

RUNNERS = {"single": run_single, "sweep": run_sweep, "datatable": run_datatable,
           "pair": run_pair, "covert": run_covert, "sidechannel": run_sidechannel}

def run_experiment(exp):
    RUNNERS[exp["kind"]](exp)

def read_key():
    import termios, tty
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        ch = sys.stdin.read(1)
        if ch == "\x1b":
            if sys.stdin.read(1) == "[":
                return {"A": "up", "B": "down", "C": "right", "D": "left"}.get(sys.stdin.read(1), "esc")
            return "esc"
        return ch
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

def draw(sel):
    st = server_status()
    scol = {"ready": "32", "not running": "33"}.get(st, "31")
    sys.stdout.write("\x1b[2J\x1b[H")
    print("\x1b[1m  DireLeak — Artifact Evaluation\x1b[0m"
          f"        server: \x1b[{scol}m{st}\x1b[0m")
    print("  " + "─" * 60)
    for i, exp in enumerate(EXPERIMENTS):
        label = f"Artifact {i+1}: {exp['title']}"
        if i == sel:
            print(f"  \x1b[7m ▸ {label:<58}\x1b[0m")
        else:
            print(f"     {label}")
    print("  " + "─" * 60)
    print("  ↑/↓ move   Enter run   1-9 jump   r restart   s stop   a attach   q quit")

def menu():
    if not sys.stdin.isatty():
        return menu_lines()
    if not server_ready():
        sys.stdout.write("\x1b[2J\x1b[H")
        print("\x1b[1m  DireLeak — Artifact Evaluation\x1b[0m\n")
        print("  Preparing the measurement server (setup-env, build, start)…\n")
        if not ensure_server():
            input("\n  — server not ready; press Enter for the menu —")
    sel = 0
    while True:
        draw(sel)
        k = read_key()
        if k in ("up", "k"):
            sel = (sel - 1) % len(EXPERIMENTS)
        elif k in ("down", "j"):
            sel = (sel + 1) % len(EXPERIMENTS)
        elif k in ("\r", "\n"):
            sys.stdout.write("\x1b[2J\x1b[H")
            run_experiment(EXPERIMENTS[sel])
            input("\n  — press Enter to return to the menu —")
        elif k.isdigit() and 1 <= int(k) <= len(EXPERIMENTS):
            sel = int(k) - 1
            sys.stdout.write("\x1b[2J\x1b[H")
            run_experiment(EXPERIMENTS[sel])
            input("\n  — press Enter to return to the menu —")
        elif k == "r":
            sys.stdout.write("\x1b[2J\x1b[H")
            if session_exists():
                kill_session(); time.sleep(0.5)
            ensure_server()
            input("\n  — press Enter to return to the menu —")
        elif k == "s":
            stop_server(); print("  server stopped."); time.sleep(0.6)
        elif k == "a":
            draw(sel); print(f"\n  attach: tmux attach -t {SESSION}  (detach: Ctrl-b d)")
            input("  — Enter —")
        elif k in ("q", "\x03"):
            if session_exists():
                print(f"\nLeaving server running (attach: tmux attach -t {SESSION}).")
            else:
                print("\nServer stopped. Bye.")
            return

def menu_lines():
    if not server_ready():
        ensure_server()
    while True:
        print("\n" + "═" * 60 + f"\n  DireLeak AE   server: {server_status()}\n" + "═" * 60)
        for i, exp in enumerate(EXPERIMENTS, 1):
            print(f"  {i}) Artifact {i}: {exp['title']}")
        print("  r) restart  s) stop  q) quit")
        c = sys.stdin.readline().strip().lower()
        if c in ("q", ""):
            return
        elif c == "r":
            if session_exists():
                kill_session(); time.sleep(0.5)
            ensure_server()
        elif c == "s":
            stop_server()
        elif c.isdigit() and 1 <= int(c) <= len(EXPERIMENTS):
            run_experiment(EXPERIMENTS[int(c) - 1])

def select_experiments(keys):
    if not keys or "all" in keys:
        return list(EXPERIMENTS)
    chosen = []
    for k in keys:
        hit = next((e for i, e in enumerate(EXPERIMENTS, 1)
                    if k in (e["key"], str(i))), None)
        if hit is None:
            print(f"! unknown experiment '{k}'")
        elif hit not in chosen:
            chosen.append(hit)
    return chosen

def run_batch(keys):
    exps = select_experiments(keys)
    for exp in exps:
        run_experiment(exp)
    print("\nAll requested experiments done.")
    print("__ORCH_DONE__")

def main():
    if not have_tmux():
        sys.exit("tmux is required (sudo apt-get install tmux).")
    os.chdir(REPO)
    args = sys.argv[1:]
    if args and args[0] == "--run":
        run_batch(args[1:])
        return
    menu()

if __name__ == "__main__":
    main()
