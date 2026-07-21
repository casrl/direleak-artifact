#!/bin/bash
# DireLeak counter-based covert channel (§6) — orchestrate Spy (receiver) + Trojan (sender).
#
# The Spy owns the uncore CHA HITME_HIT counters, so the msr_program server MUST
# be stopped first (it contends for those counters).  Both processes run as root
# (Spy needs /dev/cpu/*/msr; the shared hugetlbfs buffer under /dev/hugepages is
# root-owned).  The Spy creates the shared buffer, selects two mutually-congruent
# MD sets, and waits; the Trojan self-waits for spy_ready.
#
# Usage: sudo ./tools/run_covert.sh [W] [E] [PAYLOAD_BITS] [R]
#   W       spy prime lines per set   (= MD associativity; default 2)
#   E       trojan flood lines per set (default 8)
#   PAYLOAD bit string to transmit    (default 01101001, the paper's Fig-11 example)
#   R       prime/probe sub-rounds summed per bit (signal amplification; default 16)
#
# Output: output/current/covert_trace.csv   (per-window HITME_HIT trace)
# Plot  : python3 analysis/plot_covert.py    -> output/figures/covert_channel.png
set -u
cd "$(dirname "$0")/.."
W=${1:-2}; E=${2:-8}; PAYLOAD=${3:-01101001}; R=${4:-16}

command -v sudo >/dev/null && SUDO="sudo -n" || SUDO=""
$SUDO pkill -9 -x msr_program 2>/dev/null   # release the uncore CHA counters
$SUDO pkill -9 -x spy 2>/dev/null; $SUDO pkill -9 -x trojan 2>/dev/null
$SUDO rm -f /dev/hugepages/direleak_cc 2>/dev/null

# The 512MB shared buffer lives on 2MB hugetlbfs pages (mbind'd to node 0),
# so reserve enough 2MB hugepages on node 0 (256 needed + headroom).
HP=/sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
[ "$(cat "$HP" 2>/dev/null || echo 0)" -lt 288 ] && $SUDO sh -c "echo 288 > $HP" 2>/dev/null
FREE=$(cat /sys/devices/system/node/node0/hugepages/hugepages-2048kB/free_hugepages 2>/dev/null || echo 0)
[ "${FREE:-0}" -lt 256 ] && echo "[covert] WARNING: only $FREE free 2MB hugepages on node0 (need 256)"

echo "[covert] spy W=$W E=$E payload=$PAYLOAD R=$R  (msr_program server must stay stopped)"
$SUDO ./tools/spy "$W" "$E" "$PAYLOAD" "$R" &
SPY=$!
sleep 1
$SUDO ./tools/trojan &
TRO=$!

for _ in $(seq 1 300); do kill -0 $SPY 2>/dev/null || break; sleep 1; done
kill -0 $SPY 2>/dev/null && { echo "[covert] spy timeout, killing"; $SUDO kill -9 $SPY $TRO 2>/dev/null; }
wait $SPY 2>/dev/null; wait $TRO 2>/dev/null

# spy/trojan ran as root; hand output back to the invoking user so later
# (non-root) artifacts can write into output/current.
[ -n "${SUDO_USER:-}" ] && $SUDO chown -R "$SUDO_USER":"$SUDO_USER" output/current 2>/dev/null
echo "[covert] done -> output/current/covert_trace.csv"
