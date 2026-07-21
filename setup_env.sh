#!/bin/bash
# setup_env.sh — Environment preprocessing for DireLeak experiments.
# Installs dependencies, loads the MSR module, and configures the CPU for
# stable, repeatable micro-benchmark results.
#
# Must be run as root. Settings do NOT persist across reboots.
#
# Usage:
#   sudo ./setup_env.sh           # Apply all settings
#   sudo ./setup_env.sh --undo    # Restore defaults

set -euo pipefail

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()    { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
section() { echo -e "\n${CYAN}━━━ $* ━━━${NC}"; }

# ── Root check ────────────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    error "This script must be run as root (sudo $0)."
    exit 1
fi

UNDO=${1:-}

# ── Helper: expand cpulist (e.g. "0,2-4,6") → space-separated numbers ────────
expand_cpulist() {
    local list=$1 result=()
    IFS=',' read -ra parts <<< "$list"
    for part in "${parts[@]}"; do
        if [[ $part == *-* ]]; then
            local lo hi
            IFS='-' read -r lo hi <<< "$part"
            for (( i=lo; i<=hi; i++ )); do result+=("$i"); done
        else
            result+=("$part")
        fi
    done
    echo "${result[@]}"
}

# ─────────────────────────────────────────────────────────────────────────────
# 1. Install dependencies
# ─────────────────────────────────────────────────────────────────────────────
# Everything needed to build and run every artifact on a bare host:
#   build-essential pkg-config          — gcc / make / pkg-config toolchain
#   libjansson-dev libnuma-dev          — msr_program build/link deps
#   msr-tools                           — rdmsr / wrmsr
#   cpufrequtils                        — frequency pinning helpers
#   numactl                             — cross-socket victim pinning (side channel)
#   tmux                                — the orchestrator drives the server in tmux
#   librsvg2-bin                        — rsvg-convert: crisp inline SVG figures
#   python3 python3-pip python3-dev     — the analysis / victim toolchain
SYS_PKGS=(build-essential pkg-config libjansson-dev libnuma-dev msr-tools
          cpufrequtils numactl tmux librsvg2-bin python3 python3-pip python3-dev)

install_deps() {
    section "Installing system packages"

    if ! command -v apt-get &>/dev/null; then
        warn "apt-get not found; skipping automatic system-package installation."
        warn "Ensure these are installed: ${SYS_PKGS[*]}"
        return
    fi

    # Fast path: skip the apt phase when the machine is already provisioned
    # (avoids a slow/blocked 'apt-get update' on every launch). Force with
    # SETUP_SKIP_DEPS=1, or force a reinstall with SETUP_FORCE_DEPS=1.
    if [[ "${SETUP_FORCE_DEPS:-}" != "1" ]]; then
        if [[ "${SETUP_SKIP_DEPS:-}" == "1" ]] || \
           { pkg-config --exists jansson 2>/dev/null \
             && ldconfig -p 2>/dev/null | grep -q 'libnuma\.so' \
             && command -v rdmsr &>/dev/null && command -v wrmsr &>/dev/null \
             && command -v gcc &>/dev/null && command -v make &>/dev/null \
             && command -v tmux &>/dev/null && command -v numactl &>/dev/null \
             && command -v rsvg-convert &>/dev/null && command -v pip3 &>/dev/null; }; then
            info "System packages already present; skipping apt (SETUP_FORCE_DEPS=1 to reinstall)."
            return
        fi
    fi

    apt-get update -qq
    apt-get install -y "${SYS_PKGS[@]}"

    # linux-tools-$(uname -r) provides perf; best-effort
    apt-get install -y "linux-tools-$(uname -r)" 2>/dev/null \
        || apt-get install -y linux-tools-generic 2>/dev/null \
        || warn "linux-tools kernel package not found; skipping."

    ldconfig
    info "System packages installed."
}

# ─────────────────────────────────────────────────────────────────────────────
# 1b. Install Python libraries (into the interpreter that runs the analysis)
# ─────────────────────────────────────────────────────────────────────────────
install_python_deps() {
    section "Installing Python libraries"

    # SETUP_PYTHON is passed by the orchestrator (= its own sys.executable) so we
    # provision the exact interpreter that will import these libraries.
    local py="${SETUP_PYTHON:-python3}"
    command -v "$py" &>/dev/null || py=python3
    if ! command -v "$py" &>/dev/null; then
        warn "python3 not found; cannot install Python libraries."; return
    fi
    info "Target interpreter: $("$py" -c 'import sys; print(sys.executable)' 2>/dev/null || echo "$py")"

    # pip install, tolerating PEP-668 'externally-managed' system interpreters.
    pip_do() {
        "$py" -m pip install "$@" 2>&1 \
            || "$py" -m pip install --break-system-packages "$@"
    }

    # module-import-name : pip-package-name  (only install what is missing)
    local specs=(numpy:numpy matplotlib:matplotlib sklearn:scikit-learn PIL:Pillow)
    local pypi=()
    for spec in "${specs[@]}"; do
        local mod="${spec%%:*}" pkg="${spec##*:}"
        "$py" -c "import ${mod}" &>/dev/null || pypi+=("$pkg")
    done
    local tpkgs=()
    "$py" -c "import torch"       &>/dev/null || tpkgs+=(torch)
    "$py" -c "import torchvision" &>/dev/null || tpkgs+=(torchvision)

    if [[ ${#pypi[@]} -eq 0 && ${#tpkgs[@]} -eq 0 ]]; then
        info "All Python libraries already present; skipping pip."
        return
    fi

    "$py" -m pip install --upgrade pip &>/dev/null \
        || "$py" -m pip install --upgrade --break-system-packages pip &>/dev/null || true

    if [[ ${#pypi[@]} -gt 0 ]]; then
        info "Installing: ${pypi[*]}"
        pip_do "${pypi[@]}" || warn "pip failed for: ${pypi[*]}"
    fi

    # torch/torchvision: prefer the lean CPU-only wheels (this is a CPU artifact),
    # fall back to the default index if that host can't reach the CPU channel.
    if [[ ${#tpkgs[@]} -gt 0 ]]; then
        info "Installing (CPU build): ${tpkgs[*]}"
        pip_do "${tpkgs[@]}" --index-url https://download.pytorch.org/whl/cpu \
            || pip_do "${tpkgs[@]}" \
            || warn "pip failed for: ${tpkgs[*]} (install torch/torchvision manually)."
    fi
    info "Python libraries ready."
}

# ─────────────────────────────────────────────────────────────────────────────
# 2. Load MSR kernel module
# ─────────────────────────────────────────────────────────────────────────────
load_msr() {
    section "MSR kernel module"
    if lsmod | grep -q '^msr '; then
        info "msr module already loaded."
    else
        modprobe msr
        info "msr module loaded."
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 3. Disable turbo boost
# ─────────────────────────────────────────────────────────────────────────────
disable_turbo() {
    section "Disabling turbo boost"

    # intel_pstate driver (most common on modern Intel Xeon)
    if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
        info "Turbo disabled via intel_pstate/no_turbo."
        return
    fi

    # cpufreq boost sysfs (acpi-cpufreq driver)
    if [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo 0 > /sys/devices/system/cpu/cpufreq/boost
        info "Turbo disabled via cpufreq/boost."
        return
    fi

    # MSR fallback: IA32_MISC_ENABLE (0x1A0), bit 38 = IDA/Turbo Disengage
    if command -v wrmsr &>/dev/null; then
        # Read current value from CPU 0, set bit 38, write back to all CPUs
        local val
        val=$(rdmsr -p 0 0x1A0 2>/dev/null || echo "0")
        # rdmsr returns hex without 0x; use printf for safe conversion
        local val_dec new_val
        val_dec=$(printf '%d' "0x${val}")
        new_val=$(( val_dec | (1 << 38) ))
        wrmsr -a 0x1A0 "$(printf '0x%x' "$new_val")"
        info "Turbo disabled via MSR 0x1A0 bit 38 (all CPUs)."
    else
        warn "Could not disable turbo: no intel_pstate, cpufreq/boost, or wrmsr available."
    fi
}

restore_turbo() {
    section "Re-enabling turbo boost"
    if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo
        info "Turbo re-enabled via intel_pstate/no_turbo."
    elif [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo 1 > /sys/devices/system/cpu/cpufreq/boost
        info "Turbo re-enabled via cpufreq/boost."
    else
        warn "Could not re-enable turbo automatically; reboot or check BIOS."
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 4. Pin CPU frequency
# ─────────────────────────────────────────────────────────────────────────────
fix_frequency() {
    section "Pinning CPU frequency"

    # Determine base (non-turbo) frequency
    local base_freq=""
    if [[ -f /sys/devices/system/cpu/cpu0/cpufreq/base_frequency ]]; then
        base_freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/base_frequency)
        info "Base frequency (from sysfs): ${base_freq} kHz"
    else
        # Fallback: read from cpuinfo after turbo is already disabled so
        # cpuinfo_max_freq should reflect the base speed.
        base_freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null || true)
        if [[ -n "$base_freq" ]]; then
            warn "base_frequency not available; using cpuinfo_max_freq: ${base_freq} kHz"
        fi
    fi

    if [[ -z "$base_freq" ]]; then
        warn "Cannot determine base frequency; skipping frequency pinning."
        return
    fi

    local failed=0
    for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do
        [[ -d "$cpu_dir" ]] || continue

        # Set performance governor
        if [[ -f "$cpu_dir/scaling_governor" ]]; then
            echo performance > "$cpu_dir/scaling_governor" 2>/dev/null || true
        fi

        # Clamp min/max to base frequency
        # Always write max before min (avoids rejection if new min > current max)
        local hw_max hw_min
        hw_max=$(cat "$cpu_dir/cpuinfo_max_freq" 2>/dev/null || echo "$base_freq")
        hw_min=$(cat "$cpu_dir/cpuinfo_min_freq" 2>/dev/null || echo "0")
        local target
        target=$(( base_freq < hw_max ? base_freq : hw_max ))
        target=$(( target > hw_min   ? target   : hw_min  ))

        echo "$target" > "$cpu_dir/scaling_max_freq" 2>/dev/null || (( failed++ )) || true
        echo "$target" > "$cpu_dir/scaling_min_freq" 2>/dev/null || (( failed++ )) || true
    done

    if (( failed > 0 )); then
        warn "${failed} cpufreq writes failed (some CPUs may be offline or offline'd by HT disable)."
    fi
    info "CPU frequency pinned to ${base_freq} kHz (performance governor)."
}

restore_frequency() {
    section "Restoring CPU frequency scaling"
    for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do
        [[ -d "$cpu_dir" ]] || continue
        local hw_max hw_min
        hw_max=$(cat "$cpu_dir/cpuinfo_max_freq" 2>/dev/null || true)
        hw_min=$(cat "$cpu_dir/cpuinfo_min_freq" 2>/dev/null || true)
        [[ -n "$hw_max" ]] && echo "$hw_max" > "$cpu_dir/scaling_max_freq" 2>/dev/null || true
        [[ -n "$hw_min" ]] && echo "$hw_min" > "$cpu_dir/scaling_min_freq" 2>/dev/null || true
        echo powersave > "$cpu_dir/scaling_governor"    2>/dev/null \
            || echo ondemand > "$cpu_dir/scaling_governor" 2>/dev/null || true
    done
    info "CPU frequency scaling restored."
}

# ─────────────────────────────────────────────────────────────────────────────
# 5. Disable hyperthreading (SMT)
# ─────────────────────────────────────────────────────────────────────────────
disable_hyperthreading() {
    section "Disabling hyperthreading (SMT)"

    # Preferred: kernel SMT control interface (Linux 4.17+)
    if [[ -f /sys/devices/system/cpu/smt/control ]]; then
        local smt_state
        smt_state=$(cat /sys/devices/system/cpu/smt/control)
        if [[ "$smt_state" == "off" ]]; then
            info "SMT already off."
            return
        fi
        echo off > /sys/devices/system/cpu/smt/control
        info "SMT disabled via /sys/devices/system/cpu/smt/control."
        return
    fi

    # Fallback: manually offline HT siblings by CPU topology
    local disabled=0
    for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*/; do
        local topo="$cpu_dir/topology/thread_siblings_list"
        [[ -f "$topo" ]] || continue

        local cpu_id siblings primary
        cpu_id=$(basename "$cpu_dir" | tr -dc '0-9')
        siblings=$(cat "$topo")

        # Find the lowest-numbered CPU in the sibling group — that is the physical core.
        local min_sibling
        min_sibling=$(expand_cpulist "$siblings" | tr ' ' '\n' | sort -n | head -1)

        # If this logical CPU is not the physical core, offline it.
        if [[ "$cpu_id" != "$min_sibling" ]]; then
            local online_file="$cpu_dir/online"
            if [[ -f "$online_file" ]]; then
                echo 0 > "$online_file" 2>/dev/null && (( disabled++ )) || true
            fi
        fi
    done
    info "Offllined ${disabled} HT sibling CPUs."
}

restore_hyperthreading() {
    section "Re-enabling hyperthreading (SMT)"

    if [[ -f /sys/devices/system/cpu/smt/control ]]; then
        echo on > /sys/devices/system/cpu/smt/control
        info "SMT re-enabled."
        return
    fi

    local enabled=0
    for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*/; do
        local online_file="$cpu_dir/online"
        [[ -f "$online_file" ]] || continue
        echo 1 > "$online_file" 2>/dev/null && (( enabled++ )) || true
    done
    info "Re-enabled ${enabled} CPUs."
}

# ─────────────────────────────────────────────────────────────────────────────
# 6. Disable hardware prefetchers (Intel MSR 0x1A4)
# ─────────────────────────────────────────────────────────────────────────────
# MSR 0x1A4  MISC_FEATURE_CONTROL
#   Bit 0 — MLC Streamer (L2 HW prefetcher)    : 1 = disable
#   Bit 1 — MLC Spatial  (adjacent cache line) : 1 = disable
#   Bit 2 — DCU Streamer (L1 streamer)         : 1 = disable
#   Bit 3 — DCU IP       (IP-based prefetcher) : 1 = disable
# Writing 0xF disables all four on every logical CPU.
disable_prefetchers() {
    section "Disabling hardware prefetchers (MSR 0x1A4)"

    if ! command -v wrmsr &>/dev/null; then
        error "wrmsr not found. Install msr-tools and ensure the msr module is loaded."
        return 1
    fi

    # wrmsr -a broadcasts to all online CPUs
    wrmsr -a 0x1A4 0xF
    info "All four HW prefetchers disabled on all CPUs (MLC streamer, MLC spatial, DCU streamer, DCU IP)."

    # Verify on CPU 0
    local readback
    readback=$(rdmsr -p 0 0x1A4 2>/dev/null || echo "?")
    info "MSR 0x1A4 readback on CPU 0: 0x${readback}"
}

restore_prefetchers() {
    section "Re-enabling hardware prefetchers (MSR 0x1A4)"
    if command -v wrmsr &>/dev/null; then
        wrmsr -a 0x1A4 0x0
        info "HW prefetchers re-enabled."
    else
        warn "wrmsr not available; prefetchers may remain disabled until reboot."
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────
if [[ "$UNDO" == "--undo" ]]; then
    echo -e "${CYAN}=== Restoring DireLeak experiment environment to defaults ===${NC}"
    restore_prefetchers
    restore_hyperthreading
    restore_frequency
    restore_turbo
    echo -e "${CYAN}=== Restore complete ===${NC}"
else
    echo -e "${CYAN}=== DireLeak Experiment Environment Setup ===${NC}"
    install_deps
    install_python_deps
    load_msr
    disable_turbo
    fix_frequency
    disable_hyperthreading
    disable_prefetchers
    echo -e "\n${CYAN}=== Setup complete. Ready to run experiments. ===${NC}"
    echo -e "    Run: ${GREEN}sudo ./bin/msr_program${NC}"
    echo -e "    Undo: ${YELLOW}sudo ./setup_env.sh --undo${NC}"
fi
