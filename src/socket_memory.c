#include <inttypes.h>
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#include "socket_memory.h"

/* Set to 1 to enable per-probe CHA debug table in find_cha_mapped_offset. */
static int s_cha_verbose = 0;

void* address_list[MAX_SOCKETS][NUM_CHA][MAX_ADDRESSES] = {{{NULL}}};
uint8_t *socket_buffers[MAX_SOCKETS] = {NULL};

/* One representative (target) address per set — first CHA-0 address found. */
void *l1_target[L1_SETS];
void *l2_target[L2_SETS];
void *l3_target[L3_SETS];

/* Head of pointer-chase chain per set.  First 8 bytes of each cache line in
 * the chain store the next pointer (NULL at the tail). */
void *l1_eviction_sets[L1_SETS];
void *l2_eviction_sets[L2_SETS];
void *l3_eviction_sets[L3_SETS];

/* Ways populated per set — promoted to globals so benchmarks can read them via ctx. */
int l1_cnt[L1_SETS];
int l2_cnt[L2_SETS];
int l3_cnt[L3_SETS];

/* L2-demote sets (non-CHA-0; see header). */
void *l2_demote_sets[L2_SETS];
int   l2_demote_cnt[L2_SETS];

/* Forward declaration — defined in the evset save/load section below. */
static void release_extra_chunks(void);

/* Read a pagemap entry and return the physical address, or 0 if not present. */
static uint64_t pa_from_fd(int fd, uintptr_t vaddr) {
    uint64_t entry;
    pread(fd, &entry, 8, (vaddr / 4096) * 8);
    if (!(entry & (1ULL << 63))) return 0;   /* not present */
    return ((entry & ((1ULL << 54) - 1)) * 4096) | (vaddr & 0xFFF);
}

void allocate_memory_per_socket() {
    if (numa_available() < 0) {
        fprintf(stderr, "NUMA is not available on this system.\n");
        return;
    }

    int max_nodes = numa_max_node() + 1;
    if (max_nodes > MAX_SOCKETS) {
        max_nodes = MAX_SOCKETS;
    }

    int total_pages = (BUFFER_SIZE / PAGE_SIZE) * max_nodes; // Total pages across all sockets
    int processed_pages = 0;

    for (int socket_id = 0; socket_id < max_nodes; socket_id++) {
        void *raw_mem = numa_alloc_onnode(BUFFER_SIZE + ALIGNMENT, socket_id);
        if (!raw_mem) {
            fprintf(stderr, "Memory allocation failed on socket %d\n", socket_id);
            continue;
        }

        // Ensure 4MB alignment
        uintptr_t aligned_addr = ((uintptr_t)raw_mem + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
        uint8_t *buffer = (uint8_t *)aligned_addr;

        // Access every 4KB of allocated memory and show progress
        for (size_t i = 0; i < BUFFER_SIZE; i += PAGE_SIZE) {
            buffer[i] = 0;  // Enforce allocation
            
            processed_pages++;
            display_progress("Memory Allocation:", processed_pages, total_pages);
        }

        socket_buffers[socket_id] = buffer;
    }

    // Ensure progress bar reaches 100% at the end
    display_progress("Memory Allocation:", total_pages, total_pages);
    printf("\n");
}


void free_memory_per_socket() {
    release_extra_chunks();
    for (int socket_id = 0; socket_id < MAX_SOCKETS; socket_id++) {
        if (socket_buffers[socket_id]) {
            numa_free(socket_buffers[socket_id], BUFFER_SIZE);
            socket_buffers[socket_id] = NULL;
        }
    }
}

void *get_socket_buffer(int socket_id) {
    if (socket_id < 0 || socket_id >= MAX_SOCKETS) {
        fprintf(stderr, "Invalid socket ID: %d\n", socket_id);
        return NULL;
    }
    return socket_buffers[socket_id];
}

void access_flush_socket_memory_one(int socket_id) {
    void *buffer = get_socket_buffer(socket_id);
    if (!buffer) {
        fprintf(stderr, "Error: No allocated buffer for socket %d\n", socket_id);
        return;
    }

    void *target = buffer; // Targeting the first byte of the buffer

    for (int i = 0; i < 2000; i++) {
        maccess(target);  // Access the memory
        mfence();         // Memory fence
        flush(target);    // Flush from cache
        mfence();         // Memory fence
    }
}

void access_socket_memory_hitmealloc(int socket_id) {
    void *buffer = get_socket_buffer(socket_id);
    int p,q;
    if (!buffer) {
        fprintf(stderr, "Error: No allocated buffer for socket %d\n", socket_id);
        return;
    }
    if (socket_id == 0) {
        p = 1;
        q = 2;
    }
    else if (socket_id == 1) {
        p = 2;
        q = 3;
    }
    else if (socket_id == 2) {
        p = 3;
        q = 0;
    } else {
        p = 0;
        q = 1;
    }
    if (!buffer) {
        fprintf(stderr, "Error: No allocated buffer for socket %d\n", socket_id);
        return;
    }


    for (int cha = 0; cha < NUM_CHA; cha++) {
        for (int addr = 0; addr < MAX_ADDRESSES; addr++) {
            void* target = address_list[0][cha][addr];
            set_process_affinity(primary_cores[p]);
            maccess(target);  // Access the memory
            mfence();         // Memory fence
            set_process_affinity(primary_cores[q]);
            maccess(target);  // Access the memory
            mfence();         // Memory fence
        }
    }
}

/*
 * find_cha_mapped_offset – determine which CHA owns an address.
 *
 * Steps:
 *  1. Query NUMA domain of the address (get_mempolicy) to derive the
 *     physical socket and the CHA subrange for that domain:
 *       phys_socket       = numa_node / domains_per_socket
 *       cha_base          = (numa_node % domains_per_socket) * cha_per_domain
 *  2. Stimulate the HitME counter (45 access+flush cycles).
 *  3. Print a focused debug table: only the relevant socket's CHA subrange.
 *  4. Return the CHA with the highest count within that subrange.
 */
int find_cha_mapped_offset(void* address, int* msr_fds, int num_sockets,
                            cha_event_t* events, int num_events, int *socket_out)
{
    uint64_t new_counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][MAX_MONITOR_EVENTS] = {0};

    char *default_events[] = {"UNC_CHA_HITME_LOOKUP.ALL_UMASK"};
    char **event_name_list = default_events;
    int num_events_to_program = 1;

    /* ── Derive domain topology ─────────────────────────────────────────── */
    int numa_node = -1;
    get_mempolicy(&numa_node, NULL, 0, address, MPOL_F_NODE | MPOL_F_ADDR);

    int max_domains        = numa_max_node() + 1;
    int domains_per_socket = (num_sockets > 0) ? max_domains / num_sockets : 1;
    int cha_per_domain     = NUM_CHA / (domains_per_socket > 0 ? domains_per_socket : 1);
    int phys_socket        = (numa_node >= 0) ? numa_node / domains_per_socket : 0;
    int cha_base           = (numa_node >= 0) ? (numa_node % domains_per_socket) * cha_per_domain : 0;
    int cha_end            = cha_base + cha_per_domain;

    /* ── Probe ──────────────────────────────────────────────────────────── */
    freeze_counters_global(msr_fds, num_sockets);
    configure_cha_counters(msr_fds, num_sockets, events, num_events,
                           event_name_list, num_events_to_program);
    unfreeze_counters_global(msr_fds, num_sockets);

    for (int i = 0; i < 45; i++) {
        maccess(address); mfence();
        flush(address);   mfence();
    }

    freeze_counters_global(msr_fds, num_sockets);
    read_cha_counters(msr_fds, num_sockets, events, num_events,
                      event_name_list, num_events_to_program, 0, new_counts, 0);

    /* ── Debug table (only when s_cha_verbose is set) ───────────────────── */
    /*
     * Rows  = relative CHA index 0 .. cha_per_domain-1
     * Col d = count for absolute CHA (d%dps)*cpd+r on socket d/dps
     * Header notes the absolute CHA range per domain, e.g. D1(Sk0 28-55)
     */
    if (s_cha_verbose) {
        printf("[find_cha] addr=%p  domain=%d  socket=%d  CHA[%d..%d]\n",
               address, numa_node, phys_socket, cha_base, cha_end - 1);

        printf("CHA |");
        for (int d = 0; d < max_domains; d++) {
            int ds = d / domains_per_socket;
            int cb = (d % domains_per_socket) * cha_per_domain;
            printf(" D%d(Sk%d %2d-%2d) |", d, ds, cb, cb + cha_per_domain - 1);
        }
        printf("\n----+");
        for (int d = 0; d < max_domains; d++) printf("----------------+");
        printf("\n");

        for (int r = 0; r < cha_per_domain; r++) {
            printf("%3d |", r);
            int best_col = -1;
            uint64_t row_max = 0;
            for (int d = 0; d < max_domains; d++) {
                int s       = d / domains_per_socket;
                int abs_cha = (d % domains_per_socket) * cha_per_domain + r;
                uint64_t cnt = new_counts[0][s][abs_cha][0];
                if (cnt > row_max) { row_max = cnt; best_col = d; }
                printf(" %14" PRIu64 " |", cnt);
            }
            if (row_max >= 45) printf("  <-- D%d", best_col);
            printf("\n");
        }
        fflush(stdout);
    }

    /* ── Find best CHA within this domain's subrange ────────────────────── */
    int best_cha = -1;
    uint64_t max_value = 0;

    for (int c = cha_base; c < cha_end; c++) {
        if (new_counts[0][phys_socket][c][0] > max_value) {
            max_value = new_counts[0][phys_socket][c][0];
            best_cha = c;
        }
    }

    if (socket_out)
        *socket_out = (max_value >= 45) ? numa_node : -1;

    return (max_value >= 45) ? best_cha : -1;
}

void print_binary(uintptr_t value) {
    for (int i = 47; i >= 0; i--) { // Assume 48-bit addresses (typical for x86_64 virtual memory)
        if (i == 15 || i == 5) {
            printf("|"); // Separator for bits [16-6]
        }
        printf("%d", (value >> i) & 1);
    }
    printf("\n");
}

/*
 * generate_cha_mapped_offsets
 *
 * full_scan = 0 (simple): scan domain 0 only, collect CHA-0 addresses.
 *             Stops once MAX_ADDRESSES entries are found.
 *
 * full_scan = 1 (full):   interleaved scan across all domains.
 *             Collects MAX_ADDRESSES addresses per (domain, CHA) pair.
 *             Enables per-probe debug tables via s_cha_verbose.
 *             Stops once every (domain, CHA) slot is filled.
 */
void generate_cha_mapped_offsets(int* msr_fds, int num_sockets,
                                  cha_event_t* events, int num_events,
                                  int full_scan)
{
    FILE *log_file = fopen(OFFSET_FILE, "w");
    if (!log_file) { perror("Error opening log file"); return; }

    if (!full_scan) {
        /* ── Simple mode: domain 0, ALL home CHAs ───────────────────────────
         * Scan domain 0's buffer and collect MAX_ADDRESSES blocks for EVERY CHA
         * homed in domain 0.  find_cha_mapped_offset returns the block's home CHA
         * as an absolute index within domain 0's slice range [0 .. cha_per_domain).
         * The Figure-2 reproduction (benchmark10) needs the full set of blocks
         * homed in one slice (e.g. CHA 10), so we fill all 28 domain-0 CHAs here.
         */
        void *buffer = get_socket_buffer(0);
        if (!buffer) {
            fprintf(stderr, "[gen_cha] No buffer for domain 0\n");
            fclose(log_file);
            return;
        }

        /* CHA slices homed in a single NUMA domain (SPR SNC-2: 56/2 = 28). */
        int max_domains        = numa_max_node() + 1;
        if (max_domains > MAX_SOCKETS) max_domains = MAX_SOCKETS;
        int domains_per_socket = (num_sockets > 0) ? max_domains / num_sockets : 1;
        if (domains_per_socket < 1) domains_per_socket = 1;
        int cha_per_domain     = NUM_CHA / domains_per_socket;
        if (cha_per_domain < 1)       cha_per_domain = 1;
        if (cha_per_domain > NUM_CHA) cha_per_domain = NUM_CHA;

        int cnt[NUM_CHA];
        memset(cnt, 0, sizeof(cnt));
        int chas_full = 0;

        for (size_t offset = 0;
             offset < BUFFER_SIZE && chas_full < cha_per_domain;
             offset += CACHE_LINE_SIZE) {

            void *target = (char *)buffer + offset;
            int found_domain = -1;
            int cha_id = find_cha_mapped_offset(target, msr_fds, num_sockets,
                                                events, num_events, &found_domain);

            if (found_domain != 0 || cha_id < 0 || cha_id >= cha_per_domain)
                continue;
            if (cnt[cha_id] >= MAX_ADDRESSES)
                continue;

            address_list[0][cha_id][cnt[cha_id]++] = target;
            if (cnt[cha_id] == MAX_ADDRESSES)
                chas_full++;

            int total = 0;
            for (int c = 0; c < cha_per_domain; c++) total += cnt[c];
            display_progress("Find D0 CHAs: ", total, cha_per_domain * MAX_ADDRESSES);
        }
        printf("\n");

        for (int c = 0; c < cha_per_domain; c++) {
            if (cnt[c] == 0) continue;
            fprintf(log_file, "CHA %d on Domain 0:\n", c);
            for (int j = 0; j < cnt[c]; j++) {
                size_t off = (uintptr_t)address_list[0][c][j] - (uintptr_t)buffer;
                fprintf(log_file, "Offset: %zu\n", off);
            }
        }
        printf("[gen_cha] Domain 0: %d/%d CHAs filled to %d blocks each\n",
               chas_full, cha_per_domain, MAX_ADDRESSES);
        fflush(stdout);

    } else {
        /* ── Full mode: all domains, all CHAs, interleaved ──────────────── */
        int max_domains = numa_max_node() + 1;
        if (max_domains > MAX_SOCKETS) max_domains = MAX_SOCKETS;

        /* count[d][c] = addresses found so far for domain d, CHA c */
        int  cnt[MAX_SOCKETS][NUM_CHA];
        int  done[MAX_SOCKETS][NUM_CHA];
        memset(cnt,  0, sizeof(cnt));
        memset(done, 0, sizeof(done));

        /* Mark domains without a buffer as fully done */
        for (int d = 0; d < max_domains; d++) {
            if (!get_socket_buffer(d)) {
                fprintf(stderr, "[gen_cha] No buffer for domain %d\n", d);
                for (int c = 0; c < NUM_CHA; c++) done[d][c] = 1;
            }
        }

        s_cha_verbose = 1;

        for (size_t offset = 0; offset < BUFFER_SIZE; offset += CACHE_LINE_SIZE) {

            int all_done = 1;
            for (int d = 0; d < max_domains && all_done; d++)
                for (int c = 0; c < NUM_CHA && all_done; c++)
                    if (!done[d][c]) all_done = 0;
            if (all_done) break;

            for (int d = 0; d < max_domains; d++) {
                void *buffer = get_socket_buffer(d);
                void *target = (char *)buffer + offset;

                int found_domain = -1;
                int cha_id = find_cha_mapped_offset(target, msr_fds, num_sockets,
                                                    events, num_events, &found_domain);

                if (cha_id >= 0 && found_domain == d && !done[d][cha_id]) {
                    address_list[d][cha_id][cnt[d][cha_id]] = target;
                    cnt[d][cha_id]++;

                    if (cnt[d][cha_id] >= MAX_ADDRESSES) {
                        done[d][cha_id] = 1;
                        fprintf(log_file, "CHA %d on Domain %d:\n", cha_id, d);
                        for (int j = 0; j < MAX_ADDRESSES; j++) {
                            size_t off = (uintptr_t)address_list[d][cha_id][j]
                                       - (uintptr_t)buffer;
                            fprintf(log_file, "Offset: %zu\n", off);
                        }
                        fflush(log_file);
                    }
                }

                /* Progress */
                printf("\r[gen_cha] offset=%-10zu  ", offset);
                for (int i = 0; i < max_domains; i++) {
                    int total = 0;
                    for (int c = 0; c < NUM_CHA; c++) total += cnt[i][c];
                    printf("D%d:%5d  ", i, total);
                }
                fflush(stdout);
            }
        }

        s_cha_verbose = 0;
        printf("\n");
    }

    fflush(log_file);
    fclose(log_file);
    printf("[gen_cha] CHA mapping completed. Results saved in %s\n", OFFSET_FILE);
    fflush(stdout);
}

void flush_targets_and_evsets(void) {
    /* Flush all target lines first. */
    for (int s = 0; s < L1_SETS; s++)
        if (l1_target[s]) { flush(l1_target[s]); mfence(); }
    for (int s = 0; s < L2_SETS; s++)
        if (l2_target[s]) { flush(l2_target[s]); mfence(); }
    for (int s = 0; s < L3_SETS; s++)
        if (l3_target[s]) { flush(l3_target[s]); mfence(); }

    /* Walk and flush every node in each eviction-set chain. */
    for (int s = 0; s < L1_SETS; s++)
        for (void *p = l1_eviction_sets[s]; p; p = *(void **)p) { flush(p); mfence(); }
    for (int s = 0; s < L2_SETS; s++)
        for (void *p = l2_eviction_sets[s]; p; p = *(void **)p) { flush(p); mfence(); }
    for (int s = 0; s < L3_SETS; s++)
        for (void *p = l3_eviction_sets[s]; p; p = *(void **)p) { flush(p); mfence(); }

    mfence();
}

/*
 * build_eviction_sets
 *
 * fast = 0 (default / slow): probes each cache line by calling
 *   find_cha_mapped_offset (freeze + configure all CHAs + unfreeze +
 *   45 access+flush + freeze + read all CHAs).  Accurate but slow.
 *
 * fast = 1: configures counters ONCE before the loop and then uses
 *   two cheap READ_MSR calls per line to detect CHA-0 ownership.
 *   Much faster but relies on low background noise.
 */
void build_eviction_sets(int *msr_fds, int num_sockets, cha_event_t *events, int num_events,
                         int fast) {
    memset(l1_target,        0, sizeof(l1_target));
    memset(l2_target,        0, sizeof(l2_target));
    memset(l3_target,        0, sizeof(l3_target));
    memset(l1_eviction_sets, 0, sizeof(l1_eviction_sets));
    memset(l2_eviction_sets, 0, sizeof(l2_eviction_sets));
    memset(l3_eviction_sets, 0, sizeof(l3_eviction_sets));
    memset(l1_cnt,           0, sizeof(l1_cnt));
    memset(l2_cnt,           0, sizeof(l2_cnt));
    memset(l3_cnt,           0, sizeof(l3_cnt));
    memset(address_list[0][0], 0, sizeof(address_list[0][0]));

    set_process_affinity(primary_cores[0]);

    void *buffer = get_socket_buffer(0);
    if (!buffer) {
        fprintf(stderr, "Error: no buffer allocated for socket 0\n");
        return;
    }

    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
        perror("Error opening /proc/self/pagemap");
        return;
    }

    /* Tail pointers for each chain — used to append new nodes in O(1). */
    void *l1_tail[L1_SETS];
    void *l2_tail[L2_SETS];
    void *l3_tail[L3_SETS];
    memset(l1_tail, 0, sizeof(l1_tail));
    memset(l2_tail, 0, sizeof(l2_tail));
    memset(l3_tail, 0, sizeof(l3_tail));

    int cha0_total = 0;

    int l1_t_done = 0, l2_t_done = 0, l3_t_done = 0;
    int l1_complete = 0, l2_complete = 0, l3_complete = 0;

    size_t l1_total_addrs = 0;
    size_t l2_total_addrs = 0;
    size_t l3_total_addrs = 0;

    /* Fast mode: configure HitME counter once; probe via delta reads. */
    char *ev_names[] = {"UNC_CHA_HITME_LOOKUP.ALL_UMASK"};
    if (fast) {
        freeze_counters_global(msr_fds, num_sockets);
        configure_cha_counters(msr_fds, num_sockets, events, num_events, ev_names, 1);
        unfreeze_counters_global(msr_fds, num_sockets);
    }

    printf("Building CHA-0 targets + eviction sets [%s] (socket 0, core %d)...\n",
           fast ? "fast" : "slow", primary_cores[0]);
    fflush(stdout);

    for (size_t offset = 0; offset < BUFFER_SIZE; offset += CACHE_LINE_SIZE) {
        /* Stop once every set has a target AND a full eviction set. */
        if (l1_t_done == L1_SETS && l1_complete == L1_SETS &&
            l2_t_done == L2_SETS && l2_complete == L2_SETS &&
            l3_t_done == L3_SETS && l3_complete == L3_SETS)
            break;

        void *addr = (char *)buffer + offset;

        /* Probe: determine CHA-0 ownership. */
        int is_cha0;
        if (!fast) {
            /* Slow: full per-line MSR probe via find_cha_mapped_offset. */
            int found_socket = -1;
            int cha_id = find_cha_mapped_offset(addr, msr_fds, num_sockets,
                                                events, num_events, &found_socket);
            is_cha0 = (cha_id == 0 && found_socket == 0);
        } else {
            /* Fast: two counter reads bracketing 200 access+flush cycles. */
            uint64_t before = 0, after = 0;
            READ_MSR(msr_fds[0], MSR_UNIT_CTR0(0), before);
            for (int i = 0; i < 200; i++) {
                maccess(addr); mfence();
                flush(addr);   mfence();
            }
            READ_MSR(msr_fds[0], MSR_UNIT_CTR0(0), after);
            is_cha0 = ((after - before) >= 200);
        }

        if (!is_cha0)
            continue;

        /* Record this CHA-0 address for benchmarks that scan address_list[0][0]. */
        if (cha0_total < MAX_ADDRESSES)
            address_list[0][0][cha0_total++] = addr;

        /* Use physical address for correct set-index extraction. */
        uint64_t paddr  = pa_from_fd(pagemap_fd, (uintptr_t)addr);
        uintptr_t l1_set = L1_SET(paddr);
        uintptr_t l2_set = L2_SET(paddr);
        uintptr_t l3_set = L3_SET(paddr);

        /* Each address gets exactly ONE role — first unfilled slot wins.
         * Eviction-set nodes are pointer-written (*(void**)addr = next);
         * targets must never be written, so they cannot share an address
         * with any eviction-set node.                                       */
        if (!l3_target[l3_set]) {
            l3_target[l3_set] = addr;
            l3_t_done++;
        } else if (l3_cnt[l3_set] < L3_WAYS) {
            *(void **)addr = NULL;
            if (l3_cnt[l3_set] == 0)
                l3_eviction_sets[l3_set] = addr;
            else
                *(void **)l3_tail[l3_set] = addr;
            l3_tail[l3_set] = addr;
            l3_cnt[l3_set]++;
            l3_total_addrs++;
            if (l3_cnt[l3_set] == L3_WAYS) l3_complete++;
        } else if (!l2_target[l2_set]) {
            l2_target[l2_set] = addr;
            l2_t_done++;
        } else if (l2_cnt[l2_set] < L2_WAYS) {
            *(void **)addr = NULL;
            if (l2_cnt[l2_set] == 0)
                l2_eviction_sets[l2_set] = addr;
            else
                *(void **)l2_tail[l2_set] = addr;
            l2_tail[l2_set] = addr;
            l2_cnt[l2_set]++;
            l2_total_addrs++;
            if (l2_cnt[l2_set] == L2_WAYS) l2_complete++;
        } else if (!l1_target[l1_set]) {
            l1_target[l1_set] = addr;
            l1_t_done++;
        } else if (l1_cnt[l1_set] < L1_WAYS) {
            *(void **)addr = NULL;
            if (l1_cnt[l1_set] == 0)
                l1_eviction_sets[l1_set] = addr;
            else
                *(void **)l1_tail[l1_set] = addr;
            l1_tail[l1_set] = addr;
            l1_cnt[l1_set]++;
            l1_total_addrs++;
            if (l1_cnt[l1_set] == L1_WAYS) l1_complete++;
        }

        printf("\rTargets L1:%4d/%-4d L2:%4d/%-4d L3:%4d/%-4d  "
               "EvictFull L1:%4d/%-4d L2:%4d/%-4d L3:%4d/%-4d  [%4zu MB]",
               l1_t_done, L1_SETS, l2_t_done, L2_SETS, l3_t_done, L3_SETS,
               l1_complete, L1_SETS, l2_complete, L2_SETS, l3_complete, L3_SETS,
               offset >> 20);
        fflush(stdout);
    }

    if (fast)
        freeze_counters_global(msr_fds, num_sockets);
    close(pagemap_fd);

    printf("EvictFull:  L1 %d/%d (addrs=%zu)  L2 %d/%d (addrs=%zu)  "
           "L3 %d/%d (addrs=%zu)\n",
           l1_complete, L1_SETS, l1_total_addrs,
           l2_complete, L2_SETS, l2_total_addrs,
           l3_complete, L3_SETS, l3_total_addrs);
    fflush(stdout);

    assert(l1_complete == L1_SETS && "L1 eviction sets incomplete");
    assert(l2_complete == L2_SETS && "L2 eviction sets incomplete");
    assert(l3_complete == L3_SETS && "L3 eviction sets incomplete");
}

/* ─────────────────────────────────────────────────────────────────────────
 * build_l2_demote_sets — per L2 set, a pointer-chase chain of L2_DEMOTE_WAYS
 * lines that share the L2-set index (PA[6:16]) but map to a CHA != 0.
 * Accessing the chain after a CHA-0 line B (with that L2 set) evicts B from the
 * private 16-way L2, so B victim-fills into its CHA-0 L3 slice — while the
 * demote lines themselves fill OTHER slices, never B's L3 set. This separates
 * "demote into L3" from "evict from L3", which l2_eviction_sets cannot do
 * (they are CHA-0 and so partly alias the target's own L3 set).
 * ───────────────────────────────────────────────────────────────────────── */
#define L2_DEMOTE_WAYS 24   /* > L2_WAYS(16) for margin against PLRU */
void build_l2_demote_sets(int *msr_fds, int num_sockets, cha_event_t *events,
                          int num_events, int fast) {
    (void)fast;
    memset(l2_demote_sets, 0, sizeof(l2_demote_sets));
    memset(l2_demote_cnt,  0, sizeof(l2_demote_cnt));
    set_process_affinity(primary_cores[0]);

    void *buffer = get_socket_buffer(0);
    if (!buffer) { fprintf(stderr, "[l2demote] no socket-0 buffer\n"); return; }
    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) { perror("[l2demote] pagemap"); return; }

    void *tail[L2_SETS]; memset(tail, 0, sizeof(tail));
    int sets_full = 0;

    /* fast CHA-0 detection via the HitMe counter delta (same trick as build). */
    char *ev_names[] = {"UNC_CHA_HITME_LOOKUP.ALL_UMASK"};
    freeze_counters_global(msr_fds, num_sockets);
    configure_cha_counters(msr_fds, num_sockets, events, num_events, ev_names, 1);
    unfreeze_counters_global(msr_fds, num_sockets);

    printf("Building L2-demote sets (non-CHA-0, %d ways/set)...\n", L2_DEMOTE_WAYS);
    fflush(stdout);

    for (size_t offset = 0; offset < BUFFER_SIZE && sets_full < L2_SETS;
         offset += CACHE_LINE_SIZE) {
        void *addr = (char *)buffer + offset;

        uint64_t before = 0, after = 0;
        READ_MSR(msr_fds[0], MSR_UNIT_CTR0(0), before);
        for (int i = 0; i < 200; i++) { maccess(addr); mfence(); flush(addr); mfence(); }
        READ_MSR(msr_fds[0], MSR_UNIT_CTR0(0), after);
        if ((after - before) >= 200) continue;          /* this IS CHA-0 — skip */

        uint64_t pa = pa_from_fd(pagemap_fd, (uintptr_t)addr);
        uintptr_t l2s = L2_SET(pa);
        if (l2_demote_cnt[l2s] >= L2_DEMOTE_WAYS) continue;

        *(void **)addr = NULL;
        if (l2_demote_cnt[l2s] == 0) l2_demote_sets[l2s] = addr;
        else                         *(void **)tail[l2s] = addr;
        tail[l2s] = addr;
        if (++l2_demote_cnt[l2s] == L2_DEMOTE_WAYS) sets_full++;

        if ((offset & 0xFFFFF) == 0) {
            printf("\r[l2demote] %d/%d sets full  [%4zu MB]", sets_full, L2_SETS, offset >> 20);
            fflush(stdout);
        }
    }
    close(pagemap_fd);
    freeze_counters_global(msr_fds, num_sockets);
    printf("\r[l2demote] complete: %d/%d L2 sets have %d non-CHA-0 demote ways\n",
           sets_full, L2_SETS, L2_DEMOTE_WAYS);
    fflush(stdout);
}

/* ─────────────────────────────────────────────────────────────────────────
 * l2_demote — the L2-demote primitive (operation).
 *
 * Ensure `line` is resident, then evict it from its private (per-core) L2 set
 * by walking the non-CHA-0 demote chain built by build_l2_demote_sets(). On
 * SPR's NON-INCLUSIVE L3 this victim-fills `line` from L2 into its own L3
 * slice WITHOUT touching CHA-0's L3 sets — the demote lines map to other
 * slices — avoiding the conflation of reusing the CHA-0 l2_eviction_sets.
 *
 * Other than the access of `line`, a no-op if the demote chain for that L2 set
 * was not populated. Caller must have run build_l2_demote_sets() and be pinned
 * to the intended core.
 * ───────────────────────────────────────────────────────────────────────── */
void l2_demote(void *line) {
    uintptr_t l2s = L2_SET(get_pa((uintptr_t)line));
    maccess(line); mfence();
    void *p = l2_demote_sets[l2s];
    for (int i = 0; i < l2_demote_cnt[l2s] && p; i++) {
        void *next = *(void **)p;
        maccess(p); mfence();
        p = next;
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Eviction-set save / load
 * ─────────────────────────────────────────────────────────────────────────
 *
 * File layout:
 *   EvsetHdr  (16 bytes)  — magic, version, n_records
 *   EvsetRec[]            — one per target or evset entry
 *
 * For each record we store:
 *   level  : 1=L1, 2=L2, 3=L3
 *   role   : 0=target, 1=evset
 *   set    : set index within that level
 *   offset : byte offset of the virtual address from socket_buffers[0]
 *             (or UINT64_MAX if the address lives in an extra chunk — stored PA only)
 *   pa     : physical address (must match pagemap on load)
 */

#define EVSET_MAGIC   0x455653455420ULL   /* "EVSET " */
/* v2: SPR L3 set-index changed 4096->2048 (PA[6:16]); v1 files hold L3 set
 * indices up to 4095 and would write out of bounds — reject & rebuild them. */
#define EVSET_VERSION 2U
#define EVSET_CHUNK_SIZE (256UL * 1024 * 1024)  /* 256 MB per extra chunk */

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t version;
    uint32_t n_records;
} EvsetHdr;

typedef struct __attribute__((packed)) {
    uint8_t  level;     /* 1/2/3 */
    uint8_t  role;      /* 0=target, 1=evset */
    uint16_t pad;
    uint32_t set;
    uint64_t offset;    /* VA offset from socket_buffers[0]; UINT64_MAX if unknown */
    uint64_t pa;        /* physical address */
} EvsetRec;

/* Extra chunks allocated during load to find missing pages. */
#define MAX_EXTRA_CHUNKS 4000
static void   *g_extra_chunks[MAX_EXTRA_CHUNKS];
static size_t  g_extra_sizes[MAX_EXTRA_CHUNKS];
static int     g_n_extra_chunks = 0;

static void release_extra_chunks(void) {
    for (int i = 0; i < g_n_extra_chunks; i++) {
        if (g_extra_chunks[i]) {
            numa_free(g_extra_chunks[i], g_extra_sizes[i]);
            g_extra_chunks[i] = NULL;
        }
    }
    g_n_extra_chunks = 0;
}

/* ── save ──────────────────────────────────────────────────────────────── */
int save_eviction_sets(void) {
    mkdir("evset", 0755);   /* ignore error if already exists */

    FILE *fp = fopen(EVSET_FILE, "wb");
    if (!fp) { perror("[evset] fopen save"); return 0; }

    /* Write placeholder header; will rewind and update n_records at the end. */
    EvsetHdr hdr = { EVSET_MAGIC, EVSET_VERSION, 0 };
    fwrite(&hdr, sizeof(hdr), 1, fp);

    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) { perror("[evset] open pagemap"); fclose(fp); return 0; }

    uint8_t *buf = socket_buffers[0];
    uint32_t nrec = 0;

    /* Helper lambda (as inline macro): write one record. */
#define WRITE_REC(lvl, rl, si, vaddr) do {                              \
    uintptr_t _va = (uintptr_t)(vaddr);                                 \
    uint64_t  _pa = pa_from_fd(pagemap_fd, _va);                        \
    uint64_t  _off = (_va >= (uintptr_t)buf &&                          \
                      _va <  (uintptr_t)buf + BUFFER_SIZE)              \
                     ? (_va - (uintptr_t)buf) : (uint64_t)UINT64_MAX;  \
    EvsetRec _r = { (lvl), (rl), 0, (si), _off, _pa };                 \
    fwrite(&_r, sizeof(_r), 1, fp);                                     \
    nrec++;                                                             \
} while(0)

    /* L3 targets + evsets */
    for (int s = 0; s < L3_SETS; s++) {
        if (l3_target[s])        WRITE_REC(3, 0, s, l3_target[s]);
        void *p = l3_eviction_sets[s];
        while (p) { WRITE_REC(3, 1, s, p); p = *(void **)p; }
    }
    /* L2 targets + evsets */
    for (int s = 0; s < L2_SETS; s++) {
        if (l2_target[s])        WRITE_REC(2, 0, s, l2_target[s]);
        void *p = l2_eviction_sets[s];
        while (p) { WRITE_REC(2, 1, s, p); p = *(void **)p; }
    }
    /* L1 targets + evsets */
    for (int s = 0; s < L1_SETS; s++) {
        if (l1_target[s])        WRITE_REC(1, 0, s, l1_target[s]);
        void *p = l1_eviction_sets[s];
        while (p) { WRITE_REC(1, 1, s, p); p = *(void **)p; }
    }

#undef WRITE_REC

    close(pagemap_fd);

    /* Rewrite header with actual record count. */
    rewind(fp);
    hdr.n_records = nrec;
    fwrite(&hdr, sizeof(hdr), 1, fp);
    fclose(fp);

    printf("[evset] Saved %u records to %s\n", nrec, EVSET_FILE);
    return 1;
}

/* ── helpers for load ──────────────────────────────────────────────────── */

/* PA + record-index pair used for sorted lookup in extra chunks. */
typedef struct { uint64_t pa; uint32_t idx; } PAIdx;

static int cmp_paidx(const void *a, const void *b) {
    const PAIdx *x = (const PAIdx *)a, *y = (const PAIdx *)b;
    if (x->pa < y->pa) return -1;
    if (x->pa > y->pa) return  1;
    return 0;
}

/* First index in sorted array where pa >= target_pa (lower bound). */
static uint32_t lower_bound_paidx(const PAIdx *arr, uint32_t n, uint64_t target_pa) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (arr[mid].pa < target_pa) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Try to assign a found virtual address to the appropriate evset/target slot.
 * Returns 1 if the record was accepted, 0 if the slot is already filled. */
static int try_assign(const EvsetRec *rec, uintptr_t va,
                      void **l1_tail, void **l2_tail, void **l3_tail) {
    uint32_t s = rec->set;
    if (rec->level == 3) {
        if (rec->role == 0) {
            if (l3_target[s]) return 0;
            l3_target[s] = (void *)va;
            return 1;
        } else {
            if (l3_cnt[s] >= L3_WAYS) return 0;
            *(void **)va = NULL;
            if (l3_cnt[s] == 0) l3_eviction_sets[s] = (void *)va;
            else                *(void **)l3_tail[s]  = (void *)va;
            l3_tail[s] = (void *)va;
            l3_cnt[s]++;
            return 1;
        }
    } else if (rec->level == 2) {
        if (rec->role == 0) {
            if (l2_target[s]) return 0;
            l2_target[s] = (void *)va;
            return 1;
        } else {
            if (l2_cnt[s] >= L2_WAYS) return 0;
            *(void **)va = NULL;
            if (l2_cnt[s] == 0) l2_eviction_sets[s] = (void *)va;
            else                *(void **)l2_tail[s]  = (void *)va;
            l2_tail[s] = (void *)va;
            l2_cnt[s]++;
            return 1;
        }
    } else { /* L1 */
        if (rec->role == 0) {
            if (l1_target[s]) return 0;
            l1_target[s] = (void *)va;
            return 1;
        } else {
            if (l1_cnt[s] >= L1_WAYS) return 0;
            *(void **)va = NULL;
            if (l1_cnt[s] == 0) l1_eviction_sets[s] = (void *)va;
            else                *(void **)l1_tail[s]  = (void *)va;
            l1_tail[s] = (void *)va;
            l1_cnt[s]++;
            return 1;
        }
    }
}

/* Scan a contiguous memory region [region_start, region_start+region_size)
 * for physical pages matching the outstanding PAIdx entries.
 * Returns number of new records resolved. */
static int scan_region_for_pas(int pagemap_fd,
                                uintptr_t region_start, size_t region_size,
                                PAIdx *sorted, uint32_t n_sorted, int *resolved,
                                const EvsetRec *recs,
                                void **l1_tail, void **l2_tail, void **l3_tail) {
    int found = 0;
    /* Walk page by page. */
    for (size_t pg = 0; pg < region_size; pg += 4096) {
        uintptr_t page_va  = region_start + pg;
        uint64_t  entry;
        if (pread(pagemap_fd, &entry, 8, (page_va / 4096) * 8) != 8) continue;
        if (!(entry & (1ULL << 63))) continue;   /* not present */
        uint64_t pfn      = entry & ((1ULL << 54) - 1);
        uint64_t page_pa  = pfn * 4096;

        /* Binary-search for any saved PA that falls in this page. */
        uint32_t lo = lower_bound_paidx(sorted, n_sorted, page_pa);
        while (lo < n_sorted && sorted[lo].pa < page_pa + 4096) {
            uint32_t ridx = sorted[lo].idx;
            if (resolved[ridx] == 0) {
                uint64_t offset_in_page = recs[ridx].pa & 0xFFF;
                uintptr_t va = page_va + offset_in_page;
                if (try_assign(&recs[ridx], va, l1_tail, l2_tail, l3_tail)) {
                    resolved[ridx] = 1;
                    found++;
                }
            }
            lo++;
        }
    }
    return found;
}

/* ── load ──────────────────────────────────────────────────────────────── */
int load_eviction_sets(void) {
    FILE *fp = fopen(EVSET_FILE, "rb");
    if (!fp) return 0;   /* no saved file — caller will rebuild */

    EvsetHdr hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
        hdr.magic != EVSET_MAGIC || hdr.version != EVSET_VERSION) {
        fclose(fp);
        printf("[evset] Invalid or stale evset file — will rebuild\n");
        return 0;
    }

    uint32_t nrecs = hdr.n_records;
    EvsetRec *recs = malloc(nrecs * sizeof(EvsetRec));
    if (!recs) { fclose(fp); return 0; }
    if (fread(recs, sizeof(EvsetRec), nrecs, fp) != nrecs) {
        fclose(fp); free(recs); return 0;
    }
    fclose(fp);

    printf("[evset] Loading %u records from %s\n", nrecs, EVSET_FILE);

    /* Zero out all target / evset arrays (they may have stale data). */
    memset(l1_target, 0, sizeof(l1_target));
    memset(l2_target, 0, sizeof(l2_target));
    memset(l3_target, 0, sizeof(l3_target));
    memset(l1_eviction_sets, 0, sizeof(l1_eviction_sets));
    memset(l2_eviction_sets, 0, sizeof(l2_eviction_sets));
    memset(l3_eviction_sets, 0, sizeof(l3_eviction_sets));
    memset(l1_cnt, 0, sizeof(l1_cnt));
    memset(l2_cnt, 0, sizeof(l2_cnt));
    memset(l3_cnt, 0, sizeof(l3_cnt));

    /* Tail-pointer arrays for pointer-chase chain construction. */
    void *l1_tail[L1_SETS]; memset(l1_tail, 0, sizeof(l1_tail));
    void *l2_tail[L2_SETS]; memset(l2_tail, 0, sizeof(l2_tail));
    void *l3_tail[L3_SETS]; memset(l3_tail, 0, sizeof(l3_tail));

    int *resolved = calloc(nrecs, sizeof(int));
    if (!resolved) { free(recs); return 0; }

    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) { perror("[evset] open pagemap"); free(recs); free(resolved); return 0; }

    int found = 0;

    /* ── Phase 1: check primary buffer with saved offset hint ── */
    uint8_t *buf = socket_buffers[0];
    for (uint32_t i = 0; i < nrecs; i++) {
        if (recs[i].offset == UINT64_MAX) continue;  /* no hint */
        uintptr_t va = (uintptr_t)buf + recs[i].offset;
        if (va < (uintptr_t)buf || va >= (uintptr_t)buf + BUFFER_SIZE) continue;
        uint64_t pa = pa_from_fd(pagemap_fd, va);
        if (pa == recs[i].pa) {
            if (try_assign(&recs[i], va, l1_tail, l2_tail, l3_tail)) {
                resolved[i] = 1;
                found++;
            }
        }
    }
    printf("[evset] Phase 1 (offset hint): %d/%u resolved\n", found, nrecs);

    /* ── Phase 1b: scan primary buffer for remaining records ── */
    if (found < (int)nrecs) {
        /* Build sorted PAIdx array for unresolved records. */
        uint32_t n_unresolved = nrecs - (uint32_t)found;
        PAIdx *sorted = malloc(n_unresolved * sizeof(PAIdx));
        if (sorted) {
            uint32_t j = 0;
            for (uint32_t i = 0; i < nrecs; i++) {
                if (!resolved[i]) { sorted[j].pa = recs[i].pa; sorted[j].idx = i; j++; }
            }
            qsort(sorted, n_unresolved, sizeof(PAIdx), cmp_paidx);

            int new_found = scan_region_for_pas(pagemap_fd,
                                (uintptr_t)buf, BUFFER_SIZE,
                                sorted, n_unresolved, resolved,
                                recs, l1_tail, l2_tail, l3_tail);
            found += new_found;
            printf("[evset] Phase 1b (primary buffer scan): +%d resolved (%d/%u total)\n",
                   new_found, found, nrecs);
            free(sorted);
        }
    }

    /* ── Phase 2: allocate extra chunks to find remaining records ── */
    if (found < (int)nrecs) {
        /* Trigger memory compaction to drain per-CPU page-allocator free lists. */
        FILE *pf;
        pf = fopen("/proc/sys/vm/compact_memory", "w");
        if (pf) { fprintf(pf, "1\n"); fclose(pf); }
        pf = fopen("/proc/sys/vm/drop_caches", "w");
        if (pf) { fprintf(pf, "3\n"); fclose(pf); }
        printf("[evset] Memory compacted — hunting %d remaining PAs\n", nrecs - found);

        /* 80% of free NUMA node-0 memory as safety cap. */
        long long free_bytes = 0;
        numa_node_size64(0, &free_bytes);
        size_t safe_extra = (size_t)((double)free_bytes * 0.80);
        size_t total_extra = 0;

        while (found < (int)nrecs) {
            if (total_extra + EVSET_CHUNK_SIZE > safe_extra) {
                printf("[evset] Reached safe allocation limit (%lld MB free, 80%% cap) "
                       "— %d PAs still unresolved\n",
                       free_bytes >> 20, nrecs - found);
                break;
            }
            if (g_n_extra_chunks >= MAX_EXTRA_CHUNKS) {
                printf("[evset] Extra chunk limit reached (%d chunks)\n", MAX_EXTRA_CHUNKS);
                break;
            }

            size_t alloc_sz = EVSET_CHUNK_SIZE + ALIGNMENT;
            void *raw = numa_alloc_onnode(alloc_sz, 0);
            if (!raw) {
                printf("[evset] numa_alloc_onnode failed — NUMA node 0 exhausted\n");
                break;
            }

            /* Touch every page to force physical allocation. */
            uintptr_t aligned = ((uintptr_t)raw + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
            uint8_t *chunk = (uint8_t *)aligned;
            for (size_t k = 0; k < EVSET_CHUNK_SIZE; k += 4096) chunk[k] = 0;

            g_extra_chunks[g_n_extra_chunks] = raw;
            g_extra_sizes[g_n_extra_chunks]  = alloc_sz;
            g_n_extra_chunks++;
            total_extra += EVSET_CHUNK_SIZE;

            /* Rebuild sorted array for still-unresolved records. */
            uint32_t n_unresolved = nrecs - (uint32_t)found;
            PAIdx *sorted = malloc(n_unresolved * sizeof(PAIdx));
            if (!sorted) break;
            uint32_t j = 0;
            for (uint32_t i = 0; i < nrecs; i++) {
                if (!resolved[i]) { sorted[j].pa = recs[i].pa; sorted[j].idx = i; j++; }
            }
            qsort(sorted, n_unresolved, sizeof(PAIdx), cmp_paidx);

            int new_found = scan_region_for_pas(pagemap_fd,
                                (uintptr_t)chunk, EVSET_CHUNK_SIZE,
                                sorted, n_unresolved, resolved,
                                recs, l1_tail, l2_tail, l3_tail);
            found += new_found;
            free(sorted);

            printf("[evset] Chunk %d (+%zu MB): +%d resolved (%d/%u total)\n",
                   g_n_extra_chunks, total_extra >> 20, new_found, found, nrecs);
        }
    }

    close(pagemap_fd);
    free(recs);
    free(resolved);

    /* Verify all L3/L2/L1 sets are complete. */
    int l3_complete = 0, l2_complete = 0, l1_complete = 0;
    for (int s = 0; s < L3_SETS; s++) if (l3_target[s] && l3_cnt[s] == L3_WAYS) l3_complete++;
    for (int s = 0; s < L2_SETS; s++) if (l2_target[s] && l2_cnt[s] == L2_WAYS) l2_complete++;
    for (int s = 0; s < L1_SETS; s++) if (l1_target[s] && l1_cnt[s] == L1_WAYS) l1_complete++;

    printf("[evset] Load complete: L1 %d/%d  L2 %d/%d  L3 %d/%d\n",
           l1_complete, L1_SETS, l2_complete, L2_SETS, l3_complete, L3_SETS);

    int success = (l3_complete == L3_SETS && l2_complete == L2_SETS && l1_complete == L1_SETS);
    if (!success) {
        printf("[evset] Incomplete — will rebuild from scratch\n");
        /* Clean up partially-filled state so build_eviction_sets starts fresh. */
        memset(l1_target, 0, sizeof(l1_target));
        memset(l2_target, 0, sizeof(l2_target));
        memset(l3_target, 0, sizeof(l3_target));
        memset(l1_eviction_sets, 0, sizeof(l1_eviction_sets));
        memset(l2_eviction_sets, 0, sizeof(l2_eviction_sets));
        memset(l3_eviction_sets, 0, sizeof(l3_eviction_sets));
        memset(l1_cnt, 0, sizeof(l1_cnt));
        memset(l2_cnt, 0, sizeof(l2_cnt));
        memset(l3_cnt, 0, sizeof(l3_cnt));
        release_extra_chunks();
    }
    return success;
}

void access_flush_addresses(void* address_list_1d[], int num_addresses) {
    for (int i = 0; i < num_addresses; i++) {
        if (address_list_1d[i] == NULL) {
            continue;
        }

        void* target = address_list_1d[i];

        for (int j = 0; j < 1000; j++) {
            maccess(target);  // Access the memory
            mfence();         // Ensure ordering
            flush(target);    // Flush from cache
            mfence();         // Ensure completion
        }
    }
}
