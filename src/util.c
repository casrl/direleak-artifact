#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

int primary_cores[MAX_SOCKETS] = {0};  
int secondary_cores[MAX_SOCKETS] = {0};
int orchestrator_cores[MAX_SOCKETS] = {0};

uint64_t rdtsc() {
    uint64_t a, d;
    asm volatile("mfence");
    // asm volatile("rdtsc" : "=a"(a), "=d"(d));
    asm volatile("rdtscp" : "=a"(a), "=d"(d) :: "rcx");
    a = (d << 32) | a;
    return a;
}

// Usage: set_process_affinity(primary_cores[socket_id] or secondary_cores[socket_id]);
void set_process_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        perror("sched_setaffinity");
        // Optionally, you can handle the error more gracefully, like exiting or logging.
    }
}

/* Expand a cpulist string (e.g. "0-5,10,12-14") into an array of CPU ids.
 * Returns the number of CPUs written into `out` (capped at `max`). */
static int expand_cpulist(const char *cpulist, int *out, int max) {
    int count = 0;
    const char *p = cpulist;
    while (*p && count < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        int lo, hi;
        if (sscanf(p, "%d", &lo) != 1) break;
        hi = lo;
        while (*p && *p != '-' && *p != ',' && *p != '\n') p++;
        if (*p == '-') {
            p++;
            if (sscanf(p, "%d", &hi) != 1) break;
            while (*p && *p != ',' && *p != '\n') p++;
        }
        for (int i = lo; i <= hi && count < max; i++)
            out[count++] = i;
        if (*p == ',') p++;
    }
    return count;
}

/* Return 1 if cpu_id is online (or if the online file is absent, assume yes). */
static int cpu_is_online(int cpu_id) {
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/online", cpu_id);
    FILE *f = fopen(path, "r");
    if (!f) return 1;   /* cpu0 has no online file — always present */
    int val = 1;
    fscanf(f, "%d", &val);
    fclose(f);
    return val;
}

/* Discover three independent physical cores per NUMA domain and store them
 * as primary, secondary, and orchestrator.
 *
 * On SPR there are 4 NUMA domains (2 per socket); MAX_SOCKETS == 4 maps
 * 1:1 to NUMA nodes, so the arrays remain indexed by NUMA node id.
 * CPU membership is read from /sys/devices/system/node/nodeN/cpulist. */
void find_primary_secondary_cores_per_socket() {
    memset(primary_cores,      -1, sizeof(primary_cores));
    memset(secondary_cores,    -1, sizeof(secondary_cores));
    memset(orchestrator_cores, -1, sizeof(orchestrator_cores));

    srand(0x4B4B3A00U);  /* fixed seed — same cores every run */

    for (int node = 0; node < MAX_SOCKETS; node++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/node/node%d/cpulist", node);
        FILE *f = fopen(path, "r");
        if (!f) continue;   /* node doesn't exist — leave as -1 */

        char cpulist[512];
        if (!fgets(cpulist, sizeof(cpulist), f)) { fclose(f); continue; }
        fclose(f);

        int cpus[512];
        int n = expand_cpulist(cpulist, cpus, 512);

        /* Collect all online CPUs in this NUMA domain. */
        int online[512];
        int n_online = 0;
        for (int i = 0; i < n; i++) {
            if (cpu_is_online(cpus[i]))
                online[n_online++] = cpus[i];
        }

        /* Pick 3 distinct random online CPUs: primary, secondary, orchestrator. */
        if (n_online > 0) {
            int idx;

            idx = rand() % n_online;
            primary_cores[node] = online[idx];

            if (n_online > 1) {
                do { idx = rand() % n_online; }
                while (online[idx] == primary_cores[node]);
                secondary_cores[node] = online[idx];
            }

            if (n_online > 2) {
                do { idx = rand() % n_online; }
                while (online[idx] == primary_cores[node] ||
                       online[idx] == secondary_cores[node]);
                orchestrator_cores[node] = online[idx];
            }
        }
    }

    /* Print table — "NUMA Node" instead of "Socket" to reflect SPR topology. */
    printf("+------------+---------------+----------------+-------------------+\n");
    printf("| NUMA Node  | Primary Core  | Secondary Core | Orchestrator Core |\n");
    printf("+------------+---------------+----------------+-------------------+\n");
    for (int i = 0; i < MAX_SOCKETS; i++) {
        printf("| %-10d | %-13d | %-14d | %-17d |\n",
               i, primary_cores[i], secondary_cores[i], orchestrator_cores[i]);
    }
    printf("+------------+---------------+----------------+-------------------+\n");
}

void execute_on_socket_core(int socket_id, int use_secondary, void (*func)(void *), void *arg, int old_core_id) {
    if (socket_id < 0 || socket_id >= MAX_SOCKETS) {
        fprintf(stderr, "Invalid socket ID %d\n", socket_id);
        return;
    }

    int core_id = use_secondary ? secondary_cores[socket_id] : primary_cores[socket_id];
    if (core_id == -1) {
        fprintf(stderr, "No suitable core found for socket %d\n", socket_id);
        return;
    }

    // Set CPU affinity to the selected core
    set_process_affinity(core_id);

    // Execute the function in the same thread
    func(arg);

    // Restore affinity to all available cores after execution
    set_process_affinity(old_core_id);
}

uint64_t get_pa(uintptr_t vaddr) {
    int fd = open("/proc/self/pagemap", O_RDONLY);
    assert(fd >= 0);
    uint64_t entry;
    ssize_t got = pread(fd, &entry, 8, (vaddr / 4096) * 8);
    assert(got == 8);
    assert(entry & (1ULL << 63));   /* page-present bit */
    close(fd);
    uint64_t pfn = entry & ((1ULL << 54) - 1);
    return (pfn * 4096) | (vaddr & 0xFFF);
}

// Function to display a progress bar with animation
void display_progress(const char *label, int current, int total) {
    static int prevPercent = -1;  // Avoid redundant prints
    if (total == 0) return;  // Prevent division by zero

    int percent = (current * 100) / total;
    if (percent == prevPercent) return;  // Skip redundant updates
    prevPercent = percent;

    int barWidth = 50;  // 50 segments for better visualization
    int filled = (percent * barWidth) / 100;

    // Create progress bar
    char bar[barWidth + 3]; // +3 for brackets and null terminator
    bar[0] = '[';
    for (int i = 1; i <= barWidth; i++) {
        if (i < filled) {
            bar[i] = '=';
        } else if (i == filled) {
            bar[i] = '>';
        } else {
            bar[i] = ' ';
        }
    }
    bar[barWidth + 1] = ']';
    bar[barWidth + 2] = '\0';

    // ASCII animation
    const char *animation[] = {"|", "/", "-", "\\"};
    const char *animChar = animation[current % 4];

    // Print progress
    printf("\r%s %s %3d%% (%d/%d) %s", label, bar, percent, current, total, animChar);
    fflush(stdout);
}

int bench_output_path(const char *name, long ts, int run, int iter,
                      const char *ext, char *buf, size_t buflen)
{
    char d[3][256];
    snprintf(d[0], sizeof(d[0]), "output");
    snprintf(d[1], sizeof(d[1]), "output/%s", name);
    snprintf(d[2], sizeof(d[2]), "output/%s/%ld", name, ts);
    for (int i = 0; i < 3; i++)
        if (mkdir(d[i], 0755) < 0 && errno != EEXIST)
            perror(d[i]);
    return snprintf(buf, buflen, "output/%s/%ld/%d_%d.%s", name, ts, run, iter, ext);
}

