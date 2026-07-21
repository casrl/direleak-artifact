#define BENCH_NAME benchmark10

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <numa.h>
#include "socket_memory.h"
#include "benchmark.h"
#include "util.h"

#define CONCAT(a, b) a##b
#define STRINGIFY(x) #x
#define EXPAND_AND_STRINGIFY(x) STRINGIFY(x)

#define TARGET_CHA_DEFAULT 9

#define N_EV 4
static const char *EV_NAME[N_EV] = {
    "UNC_CHA_HITME_LOOKUP.ALL_UMASK",
    "UNC_CHA_HITME_HIT.ALL_UMASK",
    "UNC_CHA_HITME_MISS.ALL_UMASK",
    "UNC_CHA_HITME_UPDATE.ALL_UMASK",
};

static double g_acc[MAX_SOCKETS][NUM_CHA][N_EV];
static int    g_num_sockets    = 0;
static int    g_cha_per_domain = 0;
static int    g_target         = TARGET_CHA_DEFAULT;

static int topo_num_sockets(void) {
    int map[MAX_SOCKETS] = {0};
    int ns = find_cpu_sockets(map, MAX_SOCKETS);
    if (ns <= 0)          ns = 1;
    if (ns > MAX_SOCKETS) ns = MAX_SOCKETS;
    return ns;
}

static int topo_cha_per_domain(int ns) {
    int md = numa_max_node() + 1;
    if (md > MAX_SOCKETS) md = MAX_SOCKETS;
    int dps = (ns > 0) ? md / ns : 1;
    if (dps < 1) dps = 1;
    int cpd = NUM_CHA / dps;
    if (cpd < 1) cpd = 1;
    return cpd;
}

static int resolve_target(BenchmarkCtx *ctx) {
    int t = TARGET_CHA_DEFAULT;
    if (ctx->arg && *ctx->arg) {
        char *end = NULL;
        long v = strtol(ctx->arg, &end, 10);
        if (end != ctx->arg && v >= 0 && v < g_cha_per_domain) {
            t = (int)v;
        } else {
            fprintf(stderr, "[bench10] arg '%s' not in [0,%d) — using default %d\n",
                    ctx->arg, g_cha_per_domain, t);
        }
    }
    if (t >= g_cha_per_domain) t = g_cha_per_domain - 1;
    return t;
}

static void snapshot_counters(BenchmarkCtx *ctx) {
    int ns = g_num_sockets;
    freeze_counters_global(ctx->msr_fds, ns);
    for (int s = 0; s < ns; s++) {
        if (ctx->msr_fds[s] < 0) continue;
        for (int c = 0; c < NUM_CHA; c++) {
            uint64_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
            READ_MSR(ctx->msr_fds[s], MSR_UNIT_CTR0(c), v0);
            READ_MSR(ctx->msr_fds[s], MSR_UNIT_CTR1(c), v1);
            READ_MSR(ctx->msr_fds[s], MSR_UNIT_CTR2(c), v2);
            READ_MSR(ctx->msr_fds[s], MSR_UNIT_CTR3(c), v3);
            g_acc[s][c][0] += (double)v0;
            g_acc[s][c][1] += (double)v1;
            g_acc[s][c][2] += (double)v2;
            g_acc[s][c][3] += (double)v3;
        }
    }
}

static void write_slice_csv(void) {
    int cpd = g_cha_per_domain > 0 ? g_cha_per_domain : (NUM_CHA / 2);

    char path[256];
    snprintf(path, sizeof(path), "output/current/benchmark10_slice%02d.csv", g_target);

    mkdir("output", 0755);
    mkdir("output/current", 0755);
    FILE *f = fopen(path, "w");
    if (!f) { perror("[bench10] fopen slice csv"); return; }
    fprintf(f, "event,socket,cha,domain_in_socket,logical_slice,count\n");
    for (int e = 0; e < N_EV; e++)
        for (int s = 0; s < g_num_sockets; s++)
            for (int c = 0; c < NUM_CHA; c++)
                fprintf(f, "%s,%d,%d,%d,%d,%.2f\n",
                        EV_NAME[e], s, c, c / cpd, c % cpd, g_acc[s][c][e]);
    fclose(f);
    fprintf(stderr, "[bench10] wrote %s\n", path);

    double home = g_acc[0][g_target][0], other = 0.0;
    for (int s = 0; s < g_num_sockets; s++)
        for (int c = 0; c < NUM_CHA; c++)
            if (!(s == 0 && c == g_target) && g_acc[s][c][0] > other)
                other = g_acc[s][c][0];
    fprintf(stderr,
            "[bench10] LOOKUP  S1D1 slice%d (home, fig slice %d) = %.0f   "
            "loudest other slice = %.0f   ratio = %.1fx\n",
            g_target, g_target + 1, home, other, other > 0 ? home / other : 0.0);
}

void CONCAT(BENCH_NAME, _init)(BenchmarkCtx *ctx) {
    void* (*address_list)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;

    if (ctx->run_id == 0 && ctx->iter_id == 0) {
        g_num_sockets    = topo_num_sockets();
        g_cha_per_domain = topo_cha_per_domain(g_num_sockets);
        g_target         = resolve_target(ctx);
        memset(g_acc, 0, sizeof(g_acc));
        fprintf(stderr, "[bench10] target home slice = CHA %d (fig slice %d)\n",
                g_target, g_target + 1);
    }

    set_process_affinity(ctx->primary_cores[0]);
    for (int addr = 0; addr < MAX_ADDRESSES; addr++) {
        void* target = address_list[0][g_target][addr];
        if (!target) continue;
        maccess(target);
        mfence();
    }
}

void CONCAT(BENCH_NAME, _roi)(BenchmarkCtx *ctx) {
    void* (*address_list)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;

    set_process_affinity(ctx->primary_cores[3]);
    for (int addr = 0; addr < MAX_ADDRESSES; addr++) {
        void* target = address_list[0][g_target][addr];
        if (!target) continue;
        maccess(target);
        mfence();
    }

    if (ctx->iter_id == 0) {
        snapshot_counters(ctx);
        if (ctx->run_id == NUM_RUNS - 1)
            write_slice_csv();
    }
}

void CONCAT(BENCH_NAME, _cleanup)(BenchmarkCtx *ctx) {
    void* (*address_list)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;

    for (int addr = 0; addr < MAX_ADDRESSES; addr++) {
        void* target = address_list[0][g_target][addr];
        if (!target) continue;
        flush(target);
        mfence();
    }
}

Benchmark benchmark = {
    EXPAND_AND_STRINGIFY(BENCH_NAME),
    "Fig.2/3: prime a domain-0 slice, reload cross-socket; per-slice HITME_LOOKUP (arg = slice 0-27)",
    CONCAT(BENCH_NAME, _init),
    CONCAT(BENCH_NAME, _roi),
    CONCAT(BENCH_NAME, _cleanup)
};
