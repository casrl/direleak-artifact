#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stdio.h>
#include "socket_memory.h"
#include "util.h"

/* Forward declaration so BenchmarkCtx can precede Benchmark. */
typedef struct BenchmarkCtx BenchmarkCtx;

struct BenchmarkCtx {
    /* Original data */
    void    *address_list;            /* void*(*)[NUM_CHA][MAX_ADDRESSES] */
    int     *primary_cores;
    int     *secondary_cores;
    int     *orchestrator_cores;
    /* Session tracking — set by main before every init/roi call.
     * run_id  : index within the NUM_RUNS inner loop (0-based)
     * iter_id : event-batch index (0-based)
     * timestamp: Unix seconds set once at the start of run_benchmark_session
     * arg     : optional per-invocation argument string (NULL if none). In
     *           --server mode it is the token after the benchmark name on the
     *           command line, e.g. "benchmark10 15" -> arg = "15". */
    int      run_id;
    int      iter_id;
    long     timestamp;
    const char *arg;
    /* Pre-built eviction sets — pointer-chase chain heads, one per set.
     * l1_evset[set] is the head void* of the chain; following *(void**)node
     * gives the next node; NULL terminates.  l1_cnt[set] holds the length. */
    void  **l1_evset;                /* l1_eviction_sets[L1_SETS] */
    void  **l2_evset;                /* l2_eviction_sets[L2_SETS] */
    void  **l3_evset;                /* l3_eviction_sets[L3_SETS] */
    void   **l1_target;              /* l1_target[L1_SETS]                  */
    void   **l2_target;              /* l2_target[L2_SETS]                  */
    void   **l3_target;              /* l3_target[L3_SETS]                  */
    int     *l1_cnt;                 /* l1_cnt[L1_SETS]  — ways populated   */
    int     *l2_cnt;                 /* l2_cnt[L2_SETS]                     */
    int     *l3_cnt;                 /* l3_cnt[L3_SETS]                     */
    void   **l2_demote_evset;        /* l2_demote_sets[L2_SETS] (non-CHA-0) */
    int     *l2_demote_cnt;          /* l2_demote_cnt[L2_SETS]              */
    int     *msr_fds;                /* per-socket MSR fds; msr_fds[0] reads CHA counters
                                      * (live during roi) for ground-truth probing */
};

typedef struct {
    const char *name;
    const char *description;              // short one-line summary
    void (*init)(BenchmarkCtx *ctx);
    void (*roi)(BenchmarkCtx *ctx);
    void (*cleanup)(BenchmarkCtx *ctx);
} Benchmark;

// Extern reference to benchmarks array (populated dynamically in benchmark.c)
extern Benchmark *benchmarks[];
extern int num_benchmarks;

// Function prototypes
Benchmark* get_benchmark_by_name(const char *name);
int  get_benchmark_index(const char *name);
void list_available_benchmarks();
void load_benchmarks();
int  sync_benchmarks(void);
void unload_all_benchmarks(void);

#endif // BENCHMARK_H
