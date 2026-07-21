# DireLeak: cross-socket cache timing channels exploiting the memory directory

[![GitHub contributors](https://img.shields.io/github/contributors/casrl/direleak-artifact.svg)](https://github.com/casrl/direleak-artifact/graphs/contributors/) [![Linux](https://badgen.net/static/os/linux/red)](https://badgen.net/static/os/Linux/red) [![Maintenance](https://img.shields.io/badge/Maintained%3F-yes-brightgreen.svg)](https://github.com/casrl/direleak-artifact/graphs/commit-activity) [![Language](https://img.shields.io/badge/Made%20with-C%20%2F%20Python-1f425f.svg)](https://github.com/casrl/direleak-artifact) [![Paper](https://img.shields.io/badge/Paper%20in-MICRO%202026-red.svg)](https://www.microarch.org/micro59/)

DireLeak demonstrates a **cross-socket microarchitectural timing channel** that exploits the on-chip **memory directory (MD) cache** — the **HitME cache** located in Intel's Caching and Home Agent (CHA). To reduce coherence-lookup latency, modern multi-socket servers cache frequently accessed cross-socket coherence directory entries in this shared on-die structure. Because the MD cache is **globally shared across sockets** and its state **directly modulates cross-socket memory-access latency**, an adversary running on one socket can prime and probe it to observe another socket's memory activity — **defeating socket- and NUMA-level isolation** that is widely assumed to separate mutually distrusting workloads.

This repository is the artifact for **DireLeak** (MICRO 2026). It reverse-engineers the geometry and allocation policy of the MD/HitME cache, and builds two end-to-end attacks on top of it: a **cross-domain covert channel** between a domain-isolated trojan and spy, and a **machine-learning model fingerprinting side channel**. A single menu-driven orchestrator provisions the host, drives the counter/measurement server, runs each experiment, and renders the paper's figures directly in the terminal.

## Environment

- **Operating System:** Ubuntu 24.04.4 LTS
- **Kernel:** 6.17.0-1021-generic
- **GCC:** 13.3.0
- **Python:** 3.8 (Anaconda distribution)
- **PyTorch:** 1.7.0 (CPU) — used by the side-channel victim
- **Hardware:** dual-socket **Intel Xeon Platinum 8481C** (Sapphire Rapids), **SNC-2** enabled (4 NUMA domains / 2 sockets), on a bare-metal instance. A genuine multi-socket host is required — a VM cannot exercise cross-socket coherence. The manuscript additionally evaluates Ice Lake (ICX) and Granite Rapids (GNR); the build selects the microarchitecture with `make ARCH=spr|icx|clx|skx`.

We use the following NUMA shorthand relative to a home-domain (node 0) block: `L` = node 0 (home), `R_S` = node 1 (remote, same socket), `R_D` = node 2 (remote, cross-socket), `R_D2` = node 3.

## Repository overview

- **src/**, **include/** — the counter-instrumented benchmark suite and the per-microarchitecture headers that build `bin/msr_program` (the resident measurement server) and the `benchmark*.so` plugins.
- **tools/** — the covert-channel **Spy** and **Trojan** (`spy.c`, `trojan.c`, `run_covert.sh`) and the side-channel **HitME monitor** (`hitme_monitor.c`).
- **victim/** — the PyTorch ML victim (`ml_victim.py`) and the model list (`models.py`) used as the side-channel target.
- **analysis/** — the reproduction **orchestrator** (`orchestrator.py`), the per-figure plotters, the side-channel collection/training scripts, and the in-terminal figure renderer (`termshow.py`).
- **events/** — per-generation uncore CHA counter event definitions.
- **monitor** — the HitME counter event list consumed by the measurement server.
- **setup_env.sh** — one-shot host provisioning: installs system packages and Python libraries, loads the `msr` module, and pins the CPU for stable measurements.
- **Makefile** — builds `bin/msr_program` and the benchmark plugins.

## Getting started

DireLeak is driven by a single orchestrator. From a fresh host (root required for MSR/counter access), simply run:

```sh
python3 analysis/orchestrator.py
```

On first launch this will:

1. run `setup_env.sh` to install every dependency (build toolchain, `libnuma`/`jansson`, `msr-tools`, `numactl`, `tmux`, `librsvg2-bin`, and the Python libraries: `numpy`, `matplotlib`, `scikit-learn`, `Pillow`, `torch`/`torchvision`), load the `msr` kernel module, and tune the CPU (disable turbo/SMT/prefetchers, pin frequency);
2. build `bin/msr_program` and the benchmark plugins (`make ARCH=spr`);
3. launch the measurement server inside a `tmux` session named `direleak` (attach any time with `tmux attach -t direleak`);
4. present a menu of experiments.

Selecting an experiment runs it against the live server, then prints a one-line **takeaway** and renders the corresponding **figure inline in the terminal** (crisp SVG rendering on sixel/kitty-capable terminals, with a graceful fallback otherwise). Figures are also written to `output/figures/` as both PNG and SVG.

To provision the host without launching the menu, run `sudo ./setup_env.sh` (undo the CPU tuning with `sudo ./setup_env.sh --undo`).

## Artifacts

| # | Paper | What to look for |
|---|---|---|
| 1 | Figure 2 | A single tall bar at the home slice in the S1D1 domain; every other NUMA domain stays near zero — the directory is homed on one slice. |
| 2 | Figure 3 | A clean diagonal across the 28×28 matrix: each home slice maps to exactly one observed CHA slice. |
| 3 | Figure 5 | Two clearly separated latency modes — a fast MD-hit peak and a slower MD-miss peak. The gap is the exploitable timing signal. |
| 4 | Table 3 | Residency turns on only for cross-socket transfers that leave the home holding a copy; all other transitions stay non-resident. |
| 5 | Obs. 2 | The post-`CLFLUSH` bar collapses to ~0 next to the tall no-flush bar — a single `CLFLUSH` invalidates the directory entry. |
| 6 | Figure 11 | Covert channel: the transmission set is evicted for a `1` and survives for a `0`; the decoded bit-stream matches the sent `01101001`. |
| 7 | Figure 13 | Side channel: a bright block-diagonal confusion matrix — models are separable by their cross-socket MD footprint, far above chance. |

## Hardware access

The attacks require a genuine dual-socket Intel Xeon host with the memory directory cache. If reviewers need hardware access, **coordinated (remote) access to the 2× Sapphire Rapids machine can be provided upon request.**

## Citing our paper

```bibtex
@inproceedings{direleak,
  title={DireLeak: Cross-socket Cache Timing Channels Exploiting Memory Directory},
  author={Chowdhuryy, Md Hafizul Islam and Cai, Kunbei and Zhang, Zhenkai and Zheng, Hao and Yao, Fan},
  booktitle={Proceedings of the 59th IEEE/ACM International Symposium on Microarchitecture (MICRO)},
  year={2026}
}
```
