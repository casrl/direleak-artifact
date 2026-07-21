#ifndef UTIL_H
#define UTIL_H

#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define DEBUG 0  // Set to 0 to disable debugging

#if DEBUG
    #define DEBUG_PRINT(fmt, args...) fprintf(stderr, "DEBUG: " fmt "\n", ##args)
#else
    #define DEBUG_PRINT(fmt, args...)
#endif

// ================ ARCH SPECIFIC DEFINITIONS ================ //
#if ARCH == 4   /* SPR */
#  define MAX_SOCKETS           4
#  define MAX_CORES_PER_SOCKET 28
#elif ARCH == 3 /* ICX */
#  define MAX_SOCKETS           2
#  define MAX_CORES_PER_SOCKET 32
#elif ARCH == 2 /* CLX / SKX */
#  define MAX_SOCKETS           4
#  define MAX_CORES_PER_SOCKET 28
#else
#  error "Unknown ARCH"
#endif
// ============== END ARCH SPECIFIC DEFINITIONS ============== //

#define CACHE_LINE_SIZE 64  // 64-byte alignment
#define PAGE_SIZE 4096 // 4KB pages

extern int primary_cores[MAX_SOCKETS];
extern int secondary_cores[MAX_SOCKETS];
extern int orchestrator_cores[MAX_SOCKETS];

static inline void flush(void *p) { asm volatile("clflush 0(%0)"::"r"(p): "rax"); }
static inline void maccess(void *p) { asm volatile("movq (%0), %%rax"::"r"(p): "rax"); }
static inline void mmodify(void *p) { asm volatile("movq $0x1, (%0)"::"r"(p): "memory"); }
static inline void mfence() { asm volatile("mfence"); }

uint64_t rdtsc();

void set_process_affinity(int core_id);
void find_primary_secondary_cores_per_socket();
void execute_on_socket_core(int socket_id, int use_secondary, void (*func)(void *), void *arg, int old_core_id);

void display_progress(const char *label, int current, int total);
uint64_t get_pa(uintptr_t vaddr);   // virtual → physical via /proc/self/pagemap

/*
 * Build the output file path  output/<name>/<timestamp>/<run>_<iter>.<ext>
 * and create all intermediate directories.  Writes path into buf[buflen].
 * Returns the number of characters written (like snprintf).
 */
int bench_output_path(const char *name, long ts, int run, int iter,
                      const char *ext, char *buf, size_t buflen);

#endif
