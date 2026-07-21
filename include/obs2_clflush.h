/*
 * Shared body for the Observation-2 microbenchmarks (CLFLUSH invalidates the MD
 * cache).  Two benchmarks include this — define BENCH_NAME and DO_FLUSH (0/1):
 *   benchmark13a : DO_FLUSH 0     benchmark13b : DO_FLUSH 1
 *
 *   init : allocate an MD entry — R_S(read) -> R_D(write) over the block set.
 *          DO_FLUSH additionally CLFLUSHes the blocks at the end of init.
 *   roi  : probe — read the blocks from L (home).  main.c counts the HITME
 *          events during roi, so the benchmark only performs the accesses.
 * With the entry present the probe HITs; after CLFLUSH it no longer does.
 */
#include <stdint.h>
#include <sched.h>
#include "socket_memory.h"
#include "benchmark.h"
#include "util.h"

#define _O2_CC(a, b) a##b
#define _O2_CAT(a, b) _O2_CC(a, b)
#define _O2_STR(x) #x
#define _O2_XSTR(x) _O2_STR(x)

#define OBS2_SLICE 9      /* home CHA / slice providing the blocks */
#define OBS2_NBLK  8000   /* number of blocks exercised */

static void obs2_pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof(s), &s);
    while (sched_getcpu() != cpu) sched_yield();
}

void _O2_CAT(BENCH_NAME, _init)(BenchmarkCtx *ctx) {
    void *(*al)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;
    int *pc = ctx->primary_cores;
    obs2_pin(pc[1]);                      /* R_S: seat on the home socket */
    for (int b = 0; b < OBS2_NBLK; b++) if (al[0][OBS2_SLICE][b]) maccess(al[0][OBS2_SLICE][b]);
    mfence();
    obs2_pin(pc[2]);                      /* R_D: cross-socket write -> allocate MD */
    for (int b = 0; b < OBS2_NBLK; b++) if (al[0][OBS2_SLICE][b]) (*(volatile uint64_t *)al[0][OBS2_SLICE][b])++;
    mfence();
#if DO_FLUSH
    obs2_pin(pc[0]);                      /* CLFLUSH from L -> invalidate the MD entries */
    for (int b = 0; b < OBS2_NBLK; b++) if (al[0][OBS2_SLICE][b]) flush(al[0][OBS2_SLICE][b]);
    mfence();
#endif
}

void _O2_CAT(BENCH_NAME, _roi)(BenchmarkCtx *ctx) {
    void *(*al)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;
    obs2_pin(ctx->primary_cores[0]);      /* probe from L (home) */
    for (int b = 0; b < OBS2_NBLK; b++) if (al[0][OBS2_SLICE][b]) maccess(al[0][OBS2_SLICE][b]);
    mfence();
}

void _O2_CAT(BENCH_NAME, _cleanup)(BenchmarkCtx *ctx) {
    void *(*al)[NUM_CHA][MAX_ADDRESSES] = ctx->address_list;
    for (int b = 0; b < OBS2_NBLK; b++) if (al[0][OBS2_SLICE][b]) flush(al[0][OBS2_SLICE][b]);
    mfence();
}

Benchmark benchmark = {
    _O2_XSTR(BENCH_NAME),
#if DO_FLUSH
    "Obs.2(b): alloc RS->RD then CLFLUSH in init; probe from L in roi (framework counts HITME)",
#else
    "Obs.2(a): alloc RS->RD in init; probe from L in roi (framework counts HITME)",
#endif
    _O2_CAT(BENCH_NAME, _init),
    _O2_CAT(BENCH_NAME, _roi),
    _O2_CAT(BENCH_NAME, _cleanup)
};
