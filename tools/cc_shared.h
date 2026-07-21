#ifndef CC_SHARED_H
#define CC_SHARED_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <numaif.h>
#include <time.h>

static inline long cc_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#define HUGE        (2UL << 20)
#define CC_BUFB     (512UL << 20)
#define CC_DATA_OFF HUGE
#define CC_MAXW     32
#define CC_MAXBITS  8192
#define CC_MAGIC    0x4469724c65616bULL
#define CC_SHM_PATH "/dev/hugepages/direleak_cc"

enum { CC_IDLE = 0, CC_PRIMED = 1, CC_SENT = 2, CC_PROBED = 3, CC_READY = 4 };

typedef struct {
    volatile uint64_t magic;
    volatile int spy_ready;
    volatile int trojan_ready;
    volatile int go;
    volatile int stop;

    int W;
    int E;
    int R;
    int mode;
    int use_bd;
    int oversample;
    int trojan_hold;

    int home_t, home_b;
    unsigned Xt, Xb;

    uint64_t spy_tx[CC_MAXW];
    uint64_t spy_bd[CC_MAXW];
    uint64_t troj_tx[CC_MAXW];
    uint64_t troj_bd[CC_MAXW];

    int nbits;
    unsigned char bits[CC_MAXBITS];

    volatile long win;
    volatile int  phase;
} cc_ctrl;

extern int node_cpu[4];

static inline void cc_maccess(volatile uint64_t *p) {
    asm volatile("movq (%0),%%rax" :: "r"(p) : "rax");
}
static inline void cc_mwrite(volatile uint64_t *p) {
    asm volatile("incq (%0)" :: "r"(p) : "memory", "cc");
}
static inline void cc_flush(volatile void *p) {
    asm volatile("clflush (%0)" :: "r"(p) : "memory");
}
static inline void cc_mfence(void) { asm volatile("mfence"); }
static inline void cc_lfence(void) { asm volatile("lfence"); }
static inline void cc_barrier(void) { asm volatile("" ::: "memory"); }

static inline void cc_pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof(s), &s);
    while (sched_getcpu() != cpu) sched_yield();
}

static inline void cc_ALLOC(volatile uint64_t *a) {
    cc_pin(node_cpu[1]); cc_maccess(a); cc_mfence();
    cc_pin(node_cpu[2]); cc_mwrite(a);  cc_mfence();
}

static inline void cc_ALLOC_batch(volatile uint64_t **v, int n) {
    cc_pin(node_cpu[1]); for (int i = 0; i < n; i++) cc_maccess(v[i]); cc_mfence();
    cc_pin(node_cpu[2]); for (int i = 0; i < n; i++) cc_mwrite(v[i]);  cc_mfence();
}
static inline void cc_RESET_batch(volatile uint64_t **v, int n) {
    for (int i = 0; i < n; i++) { cc_flush(v[i]); }
    cc_mfence();
}

static inline volatile uint64_t *cc_line(uint8_t *base, uint64_t off) {
    return (volatile uint64_t *)(base + off);
}

static inline uint64_t cc_mkoff(size_t pg, unsigned X, int h) {
    return pg * HUGE + (((uint64_t)X << 6) | ((uint64_t)h << 17));
}

static inline uint8_t *cc_map_shared(size_t bytes, int create) {
    int flags = O_RDWR | (create ? O_CREAT : 0);
    int fd = open(CC_SHM_PATH, flags, 0600);
    if (fd < 0) { perror("open " CC_SHM_PATH); return NULL; }
    if (create && ftruncate(fd, bytes) != 0) {  }
    uint8_t *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap " CC_SHM_PATH); return NULL; }
    if (create) {
        unsigned long mask = 1UL;
        mbind(p, bytes, MPOL_BIND, &mask, 4, MPOL_MF_MOVE);
        cc_pin(node_cpu[0]);
        for (size_t i = 0; i < bytes; i += HUGE) p[i] = 1;
        cc_mfence();
    }
    return p;
}

#endif
