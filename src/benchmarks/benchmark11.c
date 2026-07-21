#define BENCH_NAME benchmark11

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

#define PREFETCH_MSR 0x1A4UL

#define DOM_L   0
#define DOM_RS  1
#define DOM_RD  2
#define DOM_RD2 3
#define N_DOM   4

#define TARGET_CHA_DEFAULT 9
#define MAXBLK  200000
#define MAXSAMP 400000
#define FIG5_CSV "output/current/benchmark11_fig5.csv"
#define TARGET_SAMPLES 300000
#define MIN_REPS 8
#define MAX_STEPS 8

typedef struct { int dom[MAX_STEPS]; int op[MAX_STEPS]; int n; } seq_t;

static uint64_t g_hit[MAXSAMP];  static int g_nhit;
static uint64_t g_miss[MAXSAMP]; static int g_nmiss;
static uint64_t g_ovh;
static int g_num_sockets    = 0;
static int g_cha_per_domain = 0;

static int   g_slice = TARGET_CHA_DEFAULT;
static int   g_nblk  = 1;
static int   g_probe = DOM_L;
static int   g_reps  = 0;
static int   g_pre   = -1;
static int   g_order = 0;
static int   g_tgt   = 0;
static int   g_minlat = 30;
static seq_t g_seq_hit, g_seq_miss;

static void *g_pool[MAXBLK];
static int   g_npool = 0;

static int      g_pf_fd[N_DOM];
static uint64_t g_pf_save[N_DOM];
static int      g_pf_active = 0;

static inline void     mwrite(void *p)   { (*(volatile uint64_t *)p)++; }
static inline void     lfence_(void)     { asm volatile("lfence"); }
static inline uint64_t rdtscp_(void) {
    unsigned a, d, c; asm volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
    return ((uint64_t)d << 32) | a;
}
static void pin_wait(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof(s), &s);
    while (sched_getcpu() != cpu) sched_yield();
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static int topo_num_sockets(void) {
    int map[MAX_SOCKETS] = {0}; int ns = find_cpu_sockets(map, MAX_SOCKETS);
    if (ns <= 0) ns = 1; if (ns > MAX_SOCKETS) ns = MAX_SOCKETS; return ns;
}
static int topo_cha_per_domain(int ns) {
    int md = numa_max_node() + 1; if (md > MAX_SOCKETS) md = MAX_SOCKETS;
    int dps = (ns > 0) ? md / ns : 1; if (dps < 1) dps = 1;
    int cpd = NUM_CHA / dps; if (cpd < 1) cpd = 1; return cpd;
}

static void prefetch_off(BenchmarkCtx *ctx) {
    for (int d = 0; d < N_DOM; d++) {
        g_pf_fd[d] = -1; int cpu = ctx->primary_cores[d]; if (cpu < 0) continue;
        char p[64]; snprintf(p, sizeof(p), "/dev/cpu/%d/msr", cpu);
        int fd = open(p, O_RDWR); if (fd < 0) continue;
        uint64_t v = 0;
        if (pread(fd, &v, 8, PREFETCH_MSR) == 8) {
            g_pf_save[d] = v; uint64_t off = 0xF;
            if (pwrite(fd, &off, 8, PREFETCH_MSR) != 8) {}
        }
        g_pf_fd[d] = fd;
    }
    g_pf_active = 1;
}
static void prefetch_restore(void) {
    if (!g_pf_active) return;
    for (int d = 0; d < N_DOM; d++)
        if (g_pf_fd[d] >= 0) {
            if (pwrite(g_pf_fd[d], &g_pf_save[d], 8, PREFETCH_MSR) != 8) {}
            close(g_pf_fd[d]); g_pf_fd[d] = -1;
        }
    g_pf_active = 0;
}

static void parse_seq(const char *s, seq_t *q) {
    q->n = 0; if (!s || !*s) return;
    const char *p = s;
    while (*p && q->n < MAX_STEPS) {
        if (*p < '0' || *p > '3') { p++; continue; }
        int d = *p - '0'; p++;
        int o = (*p == 'w' || *p == 'W') ? 1 : 0;
        q->dom[q->n] = d; q->op[q->n] = o; q->n++;
        while (*p && *p != '-') p++;
        if (*p == '-') p++;
    }
}
static void parse_arg(const char *arg) {

    g_slice = TARGET_CHA_DEFAULT; g_nblk = 20480; g_probe = DOM_RD2; g_reps = 0;
    g_pre = -1; g_order = 0; g_tgt = 0; g_minlat = 30;
    parse_seq("1r-2w", &g_seq_hit);
    parse_seq("2r-2w", &g_seq_miss);
    if (!arg || !*arg) return;
    char buf[256]; strncpy(buf, arg, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
        char *eq = strchr(tok, '='); if (!eq) continue;
        *eq = 0; const char *k = tok, *v = eq + 1;
        if      (!strcmp(k, "s"))     g_slice = atoi(v);
        else if (!strcmp(k, "N") || !strcmp(k, "n")) g_nblk = atoi(v);
        else if (!strcmp(k, "p"))     g_probe = atoi(v);
        else if (!strcmp(k, "reps"))  g_reps  = atoi(v);
        else if (!strcmp(k, "pre"))   g_pre   = atoi(v);
        else if (!strcmp(k, "hit"))   parse_seq(v, &g_seq_hit);
        else if (!strcmp(k, "miss"))  parse_seq(v, &g_seq_miss);
        else if (!strcmp(k, "order")) g_order = !strcmp(v, "rev") ? 1 : !strcmp(v, "stride") ? 2 : 0;
        else if (!strcmp(k, "tgt"))   g_tgt   = atoi(v);
        else if (!strcmp(k, "minlat")) g_minlat = atoi(v);
    }
    if (g_nblk < 1) g_nblk = 1;
    if (g_nblk > MAXBLK) g_nblk = MAXBLK;
    if (g_probe < 0 || g_probe >= N_DOM) g_probe = DOM_L;
}

static void seq_str(const seq_t *q, char *out, int cap) {
    static const char *DN[N_DOM] = {"L", "RS", "RD", "RD2"};
    out[0] = 0; int off = 0;
    for (int i = 0; i < q->n; i++)
        off += snprintf(out + off, cap - off, "%s%s%s", i ? "->" : "",
                        DN[q->dom[i]], q->op[i] ? "w" : "r");
}

static void do_one_rep_tgt(int *core, const seq_t *seq, uint64_t *out, int *outn) {
    int cL = core[DOM_L], cP = core[g_probe];
    void *T = g_pool[0];
    pin_wait(cL); flush(T); mfence();
    for (int st = 0; st < seq->n; st++) {
        pin_wait(core[seq->dom[st]]);
        if (seq->op[st]) mwrite(T); else maccess(T);
        mfence();
    }

    pin_wait(cP);
    for (int b = 1; b < g_npool; b++) maccess(g_pool[b]);
    mfence();

    if (*outn < MAXSAMP) {
        lfence_(); uint64_t a = rdtscp_(); maccess(T); lfence_(); uint64_t z = rdtscp_();
        uint64_t lat = (z - a > g_ovh) ? (z - a - g_ovh) : 0;
        if (lat >= (uint64_t)g_minlat) out[(*outn)++] = lat;
    }
}

static void do_one_rep(int *core, const seq_t *seq, uint64_t *out, int *outn) {
    if (g_tgt) { do_one_rep_tgt(core, seq, out, outn); return; }
    int cL = core[DOM_L], cP = core[g_probe];
    pin_wait(cL);
    for (int b = 0; b < g_npool; b++) flush(g_pool[b]);
    mfence();
    for (int st = 0; st < seq->n; st++) {
        pin_wait(core[seq->dom[st]]);
        for (int b = 0; b < g_npool; b++) {
            if (seq->op[st]) mwrite(g_pool[b]); else maccess(g_pool[b]);
        }
        mfence();
    }
    if (g_pre >= 0 && g_pre < N_DOM) {
        pin_wait(core[g_pre]);
        for (int b = 0; b < g_npool; b++) maccess(g_pool[b]);
        mfence();
    }
    pin_wait(cP);
    for (int bi = 0; bi < g_npool && *outn < MAXSAMP; bi++) {
        int b = g_order == 1 ? (g_npool - 1 - bi)
              : g_order == 2 ? (int)(((unsigned)bi * 2654435761u) % (unsigned)g_npool) : bi;
        void *p = g_pool[b];
        lfence_(); uint64_t a = rdtscp_(); maccess(p); lfence_(); uint64_t z = rdtscp_();
        uint64_t lat = (z - a > g_ovh) ? (z - a - g_ovh) : 0;
        if (lat >= (uint64_t)g_minlat) out[(*outn)++] = lat;
    }
}

static uint64_t g_min, g_p05;
static void stats(uint64_t *v, int n, double *mean, uint64_t *med, uint64_t *p25, uint64_t *p75) {
    if (n <= 0) { *mean = 0; *med = *p25 = *p75 = 0; g_min = g_p05 = 0; return; }
    uint64_t *s = malloc(sizeof(uint64_t) * n);
    memcpy(s, v, sizeof(uint64_t) * n);
    qsort(s, n, sizeof(uint64_t), cmp_u64);
    double sum = 0; for (int i = 0; i < n; i++) sum += s[i];
    *mean = sum / n; *med = s[n / 2]; *p25 = s[n / 4]; *p75 = s[(3 * n) / 4];
    g_min = s[0]; g_p05 = s[n / 20];
    free(s);
}

static void write_fig5_csv(void) {
    mkdir("output", 0755); mkdir("output/current", 0755);
    FILE *f = fopen(FIG5_CSV, "w");
    if (!f) { perror("[bench11] fopen"); return; }
    fprintf(f, "scenario,latency_cycles\n");
    for (int i = 0; i < g_nhit;  i++) fprintf(f, "hit,%llu\n",  (unsigned long long)g_hit[i]);
    for (int i = 0; i < g_nmiss; i++) fprintf(f, "miss,%llu\n", (unsigned long long)g_miss[i]);
    fclose(f);

    double hm, mm; uint64_t hmed, h25, h75, mmed, m25, m75, hmin, hp05, mmin, mp05;
    stats(g_hit,  g_nhit,  &hm, &hmed, &h25, &h75); hmin = g_min; hp05 = g_p05;
    stats(g_miss, g_nmiss, &mm, &mmed, &m25, &m75); mmin = g_min; mp05 = g_p05;
    char hs[64], ms[64]; seq_str(&g_seq_hit, hs, sizeof hs); seq_str(&g_seq_miss, ms, sizeof ms);
    static const char *DN[N_DOM] = {"L", "RS", "RD", "RD2"};
    fprintf(stderr, "[bench11] cfg s=%d pool=%d probe=%s pre=%d order=%d | hit[%s] miss[%s]\n",
            g_slice, g_npool, DN[g_probe], g_pre, g_order, hs, ms);
    fprintf(stderr, "[bench11] HIT  mean=%.1f min=%llu p05=%llu med=%llu (p25=%llu p75=%llu) n=%d\n",
            hm, (unsigned long long)hmin, (unsigned long long)hp05, (unsigned long long)hmed,
            (unsigned long long)h25, (unsigned long long)h75, g_nhit);
    fprintf(stderr, "[bench11] MISS mean=%.1f min=%llu p05=%llu med=%llu (p25=%llu p75=%llu) n=%d\n",
            mm, (unsigned long long)mmin, (unsigned long long)mp05, (unsigned long long)mmed,
            (unsigned long long)m25, (unsigned long long)m75, g_nmiss);
    long long medgap = (long long)mmed - (long long)hmed;
    fprintf(stderr, "[bench11] >>> GAP mean=%.1f  med=%lld  p05=%lld  min=%lld cyc  %s\n",
            mm - hm, medgap, (long long)mp05 - (long long)hp05,
            (long long)mmin - (long long)hmin,
            (medgap >= 10 || (mm - hm) >= 10.0) ? "*** SEPARATED (median/mean gap >= 10) ***"
                                                : "(insufficient)");
    fprintf(stderr, "[bench11] wrote %s\n", FIG5_CSV);
}

void CONCAT(BENCH_NAME, _init)(BenchmarkCtx *ctx) {
    if (ctx->run_id == 0 && ctx->iter_id == 0) {
        void *(*al)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;
        g_num_sockets    = topo_num_sockets();
        g_cha_per_domain = topo_cha_per_domain(g_num_sockets);
        parse_arg(ctx->arg);
        if (g_slice < 0 || g_slice >= g_cha_per_domain) g_slice = TARGET_CHA_DEFAULT;

        g_npool = 0;
        for (int off = 0; off < g_cha_per_domain && g_npool < g_nblk; off++) {
            int sl = (g_slice + off) % g_cha_per_domain;
            for (int b = 0; b < MAX_ADDRESSES && g_npool < g_nblk && g_npool < MAXBLK; b++) {
                void *p = al[0][sl][b];
                if (p) g_pool[g_npool++] = p;
            }
        }
        if (g_reps <= 0) g_reps = g_tgt ? 4000 : TARGET_SAMPLES / (g_npool > 0 ? g_npool : 1);
        if (g_reps < MIN_REPS) g_reps = MIN_REPS;
        g_nhit = g_nmiss = 0;
        prefetch_off(ctx);

        int cP = ctx->primary_cores[g_probe];
        if (cP >= 0) pin_wait(cP);
        uint64_t ov[512];
        for (int i = 0; i < 512; i++) {
            lfence_(); uint64_t a = rdtscp_(); lfence_(); uint64_t z = rdtscp_(); ov[i] = z - a;
        }
        qsort(ov, 512, sizeof(uint64_t), cmp_u64); g_ovh = ov[256];
        fprintf(stderr, "[bench11] slice=%d pool=%d reps=%d probe-dom=%d rdtscp-ovh=%llu\n",
                g_slice, g_npool, g_reps, g_probe, (unsigned long long)g_ovh);
    }
}

void CONCAT(BENCH_NAME, _roi)(BenchmarkCtx *ctx) {
    if (ctx->run_id != 0 || ctx->iter_id != 0) return;
    if (g_npool <= 0) { fprintf(stderr, "[bench11] empty pool\n"); return; }
    int core[N_DOM];
    for (int d = 0; d < N_DOM; d++) {
        core[d] = ctx->primary_cores[d];
        if (core[d] < 0) { fprintf(stderr, "[bench11] domain %d has no core\n", d); return; }
    }
    for (int r = 0; r < g_reps; r++) {
        do_one_rep(core, &g_seq_hit,  g_hit,  &g_nhit);
        do_one_rep(core, &g_seq_miss, g_miss, &g_nmiss);
    }
    write_fig5_csv();
}

void CONCAT(BENCH_NAME, _cleanup)(BenchmarkCtx *ctx) {
    if (ctx->run_id != 0 || ctx->iter_id != 0) return;
    for (int b = 0; b < g_npool; b++) flush(g_pool[b]);
    mfence();
    prefetch_restore();
}

Benchmark benchmark = {
    EXPAND_AND_STRINGIFY(BENCH_NAME),
    "Fig.5: configurable MD hit/miss latency probe (arg s=|N=|hit=|miss=|p=|pre=|order=)",
    CONCAT(BENCH_NAME, _init),
    CONCAT(BENCH_NAME, _roi),
    CONCAT(BENCH_NAME, _cleanup)
};
