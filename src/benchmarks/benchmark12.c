#define BENCH_NAME benchmark12

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <numa.h>
#include "socket_memory.h"
#include "benchmark.h"
#include "util.h"

#define CONCAT(a, b) a##b
#define STRINGIFY(x) #x
#define EXPAND_AND_STRINGIFY(x) STRINGIFY(x)

#define DOM_L   0
#define DOM_RS  1
#define DOM_RD  2
#define DOM_RD2 3
#define N_DOM   4
#define SOCK_OF(d) ((d) < 2 ? 0 : 1)

#define CTR_HIT    1
#define CTR_LOOKUP 0

#define PREFETCH_MSR 0x1A4UL
#define RESIDENT_THRESHOLD 0.5
#define DEFAULT_BLOCKS 1000
#define DEFAULT_REPS   3
#define OUT_CSV "output/current/benchmark12_allocpolicy.csv"

static const char *DOM_NAME[N_DOM] = { "L", "RS", "RD", "RD2" };
static const char *OP_NAME[2]      = { "r", "w" };

static int g_num_sockets    = 0;
static int g_cha_per_domain = 0;
static int g_blocks         = DEFAULT_BLOCKS;
static int g_reps           = DEFAULT_REPS;

static int      g_pf_fd[N_DOM];
static uint64_t g_pf_save[N_DOM];
static int      g_pf_active = 0;

static inline void mwrite(void *p) { (*(volatile uint64_t *)p)++; }

static void pin_wait(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof(s), &s);
    while (sched_getcpu() != cpu) sched_yield();
}

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

static void resolve_args(BenchmarkCtx *ctx) {
    g_blocks = DEFAULT_BLOCKS; g_reps = DEFAULT_REPS;
    if (ctx->arg && *ctx->arg) {
        long b = strtol(ctx->arg, NULL, 10);
        if (b > 0 && b <= MAX_ADDRESSES) g_blocks = (int)b;
        const char *colon = strchr(ctx->arg, ':');
        if (colon) { long r = strtol(colon + 1, NULL, 10); if (r > 0 && r <= 100) g_reps = (int)r; }
    }
}

static void prefetch_off(BenchmarkCtx *ctx) {
    for (int d = 0; d < N_DOM; d++) {
        g_pf_fd[d] = -1;
        int cpu = ctx->primary_cores[d];
        if (cpu < 0) continue;
        char p[64]; snprintf(p, sizeof(p), "/dev/cpu/%d/msr", cpu);
        int fd = open(p, O_RDWR);
        if (fd < 0) continue;
        uint64_t v = 0;
        if (pread(fd, &v, 8, PREFETCH_MSR) == 8) {
            g_pf_save[d] = v;
            uint64_t off = 0xF;
            if (pwrite(fd, &off, 8, PREFETCH_MSR) != 8) {  }
        }
        g_pf_fd[d] = fd;
    }
    g_pf_active = 1;
}

static void prefetch_restore(void) {
    if (!g_pf_active) return;
    for (int d = 0; d < N_DOM; d++)
        if (g_pf_fd[d] >= 0) {
            if (pwrite(g_pf_fd[d], &g_pf_save[d], 8, PREFETCH_MSR) != 8) {  }
            close(g_pf_fd[d]); g_pf_fd[d] = -1;
        }
    g_pf_active = 0;
}

static double sum_ctr(BenchmarkCtx *ctx, int ctr) {
    double s = 0;
    int fd = ctx->msr_fds[0];
    if (fd < 0) return 0;
    for (int c = 0; c < g_cha_per_domain; c++) {
        uint64_t v = 0;
        uint64_t off = (ctr == CTR_HIT) ? MSR_UNIT_CTR1(c)
                     : (ctr == CTR_LOOKUP) ? MSR_UNIT_CTR0(c)
                     : MSR_UNIT_CTR2(c);
        READ_MSR(fd, off, v);
        s += (double)v;
    }
    return s;
}

static long touch_all(void *(*al)[NUM_CHA][MAX_ADDRESSES], int core, int op) {
    long n = 0;
    pin_wait(core);
    for (int c = 0; c < g_cha_per_domain; c++)
        for (int a = 0; a < g_blocks; a++) {
            void *p = al[0][c][a];
            if (!p) continue;
            if (op) mwrite(p); else maccess(p);
            n++;
        }
    mfence();
    return n;
}

static void flush_all(void *(*al)[NUM_CHA][MAX_ADDRESSES], int core_L) {
    pin_wait(core_L);
    for (int c = 0; c < g_cha_per_domain; c++)
        for (int a = 0; a < g_blocks; a++) {
            void *p = al[0][c][a];
            if (p) flush(p);
        }
    mfence();
}

static void probe(BenchmarkCtx *ctx, void *(*al)[NUM_CHA][MAX_ADDRESSES],
                  int probe_core, double *hit, double *look) {
    pin_wait(probe_core);
    double h0 = sum_ctr(ctx, CTR_HIT), l0 = sum_ctr(ctx, CTR_LOOKUP);
    for (int c = 0; c < g_cha_per_domain; c++)
        for (int a = 0; a < g_blocks; a++) {
            void *p = al[0][c][a];
            if (p) maccess(p);
        }
    mfence();
    freeze_counters_global(ctx->msr_fds, g_num_sockets);
    *hit  = sum_ctr(ctx, CTR_HIT)    - h0;
    *look = sum_ctr(ctx, CTR_LOOKUP) - l0;
    unfreeze_counters_global(ctx->msr_fds, g_num_sockets);
}

static int pick_probe(int da, int db) {
    int psock = 1 - SOCK_OF(db);
    int cand0 = psock == 0 ? DOM_L  : DOM_RD;
    int cand1 = psock == 0 ? DOM_RS : DOM_RD2;
    if (cand0 != da && cand0 != db) return cand0;
    if (cand1 != da && cand1 != db) return cand1;
    return cand0;
}

static void run_sweep(BenchmarkCtx *ctx) {
    void *(*al)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;

    int core[N_DOM];
    for (int d = 0; d < N_DOM; d++) {
        core[d] = ctx->primary_cores[d];
        if (core[d] < 0) { fprintf(stderr, "[bench12] domain %s has no core — aborting\n",
                                   DOM_NAME[d]); return; }
    }

    prefetch_off(ctx);

    double base[2];
    for (int psock = 0; psock < 2; psock++) {
        int pc = core[psock == 0 ? DOM_L : DOM_RD];
        double hit = 0, look = 0, acc = 0; long nblk = 0;
        for (int r = 0; r < g_reps; r++) {
            flush_all(al, core[DOM_L]);
            probe(ctx, al, pc, &hit, &look);
            acc += hit; nblk = 0;
            for (int c = 0; c < g_cha_per_domain; c++)
                for (int a = 0; a < g_blocks; a++) if (al[0][c][a]) nblk++;
        }
        base[psock] = nblk ? acc / (g_reps * (double)nblk) : 0.0;
    }
    fprintf(stderr, "[bench12] prefetch OFF; baseline hit/blk: socket0=%.4f socket1=%.4f "
                    "(blocks/CHA=%d, reps=%d)\n", base[0], base[1], g_blocks, g_reps);

    mkdir("output", 0755);
    mkdir("output/current", 0755);
    FILE *f = fopen(OUT_CSV, "w");
    if (!f) { perror("[bench12] fopen csv"); prefetch_restore(); return; }
    fprintf(f, "combo_id,step1,step2,owner,probe_dom,probe_socket,n_blocks,reps,"
               "hit_count,lookup_count,hit_over_lookup,"
               "hit_per_blk,baseline,residency,resident,lookup_per_blk\n");

    fprintf(stderr, "[bench12] %-11s | %-4s | probe | %10s %10s %7s | %8s | %-3s\n",
            "sequence", "own", "HIT", "LOOKUP", "H/L", "resid", "MD?");

    int combo = 0, n_alloc = 0;
    for (int da = 0; da < N_DOM; da++)
    for (int oa = 0; oa < 2;     oa++)
    for (int db = 0; db < N_DOM; db++)
    for (int ob = 0; ob < 2;     ob++, combo++) {
        int probe_dom = pick_probe(da, db);
        int psock = SOCK_OF(probe_dom);
        double acc_hit = 0, acc_look = 0; long nblk = 0;

        for (int r = 0; r < g_reps; r++) {
            double hit, look;
            flush_all(al, core[DOM_L]);
            nblk = touch_all(al, core[da], oa);
            touch_all(al, core[db], ob);
            probe(ctx, al, core[probe_dom], &hit, &look);
            acc_hit += hit; acc_look += look;
        }
        double hit_pb  = nblk ? acc_hit  / (g_reps * (double)nblk) : 0.0;
        double look_pb = nblk ? acc_look / (g_reps * (double)nblk) : 0.0;
        double ratio   = acc_look > 0 ? acc_hit / acc_look : 0.0;
        double resid   = hit_pb - base[psock];
        int resident   = resid > RESIDENT_THRESHOLD;
        if (resident) n_alloc++;

        char s1[8], s2[8];
        snprintf(s1, sizeof(s1), "%s%s", DOM_NAME[da], OP_NAME[oa]);
        snprintf(s2, sizeof(s2), "%s%s", DOM_NAME[db], OP_NAME[ob]);
        fprintf(f, "%d,%s,%s,%s,%s,%d,%ld,%d,%.0f,%.0f,%.4f,%.4f,%.4f,%.4f,%d,%.4f\n",
                combo, s1, s2, DOM_NAME[db], DOM_NAME[probe_dom], psock,
                nblk, g_reps, acc_hit, acc_look, ratio,
                hit_pb, base[psock], resid, resident, look_pb);

        char seq[24]; snprintf(seq, sizeof(seq), "%s->%s", s1, s2);
        fprintf(stderr, "[bench12] %-11s | %-4s | %-5s | %10.0f %10.0f %6.3f | %+8.3f | %s\n",
                seq, DOM_NAME[db], DOM_NAME[probe_dom], acc_hit, acc_look, ratio, resid,
                resident ? "ALLOC" : "no");
    }

    fclose(f);
    prefetch_restore();
    fprintf(stderr, "[bench12] wrote %s  (%d/%d sequences allocate an MD entry, "
                    "resident>%.1f)\n", OUT_CSV, n_alloc, combo, RESIDENT_THRESHOLD);
}

void CONCAT(BENCH_NAME, _init)(BenchmarkCtx *ctx) {
    if (ctx->run_id == 0 && ctx->iter_id == 0) {
        g_num_sockets    = topo_num_sockets();
        g_cha_per_domain = topo_cha_per_domain(g_num_sockets);
        resolve_args(ctx);
        fprintf(stderr, "[bench12] sockets=%d, home CHAs=%d, blocks/CHA=%d, reps=%d\n",
                g_num_sockets, g_cha_per_domain, g_blocks, g_reps);
    }
}

void CONCAT(BENCH_NAME, _roi)(BenchmarkCtx *ctx) {
    if (ctx->run_id == 0 && ctx->iter_id == 0)
        run_sweep(ctx);
}

void CONCAT(BENCH_NAME, _cleanup)(BenchmarkCtx *ctx) {
    if (ctx->run_id != 0 || ctx->iter_id != 0) return;
    void *(*al)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;
    for (int c = 0; c < g_cha_per_domain; c++)
        for (int a = 0; a < g_blocks; a++)
            if (al[0][c][a]) flush(al[0][c][a]);
    mfence();
}

Benchmark benchmark = {
    EXPAND_AND_STRINGIFY(BENCH_NAME),
    "exhaustive MD alloc-policy probe — 64 (domA op)->(domB op) seqs, HITME_HIT residency, opposite-owner probe + bg-subtract (arg=blocks[:reps])",
    CONCAT(BENCH_NAME, _init),
    CONCAT(BENCH_NAME, _roi),
    CONCAT(BENCH_NAME, _cleanup)
};
