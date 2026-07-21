#ifndef SOCKET_MEMORY_H
#define SOCKET_MEMORY_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <numa.h>
#include <numaif.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "msr_defs.h"
#include "util.h"

#define MATCH_THRESHOLD 10  // Number of offsets to compare for reuse

#define BUFFER_SIZE (8L * 1024 * 1024 * 1024) // 8GB per socket
// #define PAGE_SIZE (2 * 1024 * 1024)  // 2MB HugePage size
#define ALIGNMENT (2L * 1024 * 1024) // 2MB alignment

extern void* address_list[MAX_SOCKETS][NUM_CHA][MAX_ADDRESSES];
extern uint8_t *socket_buffers[MAX_SOCKETS];

/* CHA-0 / socket-0 target arrays.
 * One representative address per set — first CHA-0 address found for that set.
 * Indexed [set_index]. */
extern void *l1_target[L1_SETS];
extern void *l2_target[L2_SETS];
extern void *l3_target[L3_SETS];

/* CHA-0 / socket-0 eviction set arrays.
 * Indexed [set_index][way] — NULL entries mean that slot is not yet filled.
 * Entries are drawn from addresses that appear AFTER the corresponding target
 * in the buffer scan order. */
/* Per-set eviction chains — pointer-chase linked lists embedded in buffer memory.
 * Each element is a cache-line address whose first 8 bytes hold the next pointer
 * (NULL at the tail).  Indexed [set_index] → head of chain. */
extern void *l1_eviction_sets[L1_SETS];
extern void *l2_eviction_sets[L2_SETS];
extern void *l3_eviction_sets[L3_SETS];

/* Number of eviction-set entries populated per set (indexed [set_index]). */
extern int l1_cnt[L1_SETS];
extern int l2_cnt[L2_SETS];
extern int l3_cnt[L3_SETS];

/* L2-demote sets: per L2 set, lines sharing the L2-set index but mapping to a
 * CHA != 0. Evict a CHA-0 target/line from its (private) L2 so it victim-fills
 * into L3, WITHOUT touching CHA-0's L3 sets (demote lines land in other slices).
 * Distinct from l2_eviction_sets, which are CHA-0 and thus double as a partial
 * L3 eviction set for CHA-0 targets (the conflation). */
extern void *l2_demote_sets[L2_SETS];
extern int   l2_demote_cnt[L2_SETS];
void build_l2_demote_sets(int *msr_fds, int num_sockets, cha_event_t *events, int num_events, int fast);

/* L2-demote primitive operation: ensure `line` is resident, then evict it from
 * its private L2 set via the non-CHA-0 demote chain, victim-filling it into L3
 * (non-inclusive) WITHOUT polluting CHA-0's L3 slice. Requires
 * build_l2_demote_sets() to have run; pin to the intended core before calling. */
void l2_demote(void *line);

void allocate_memory_per_socket();
void free_memory_per_socket();
void *get_socket_buffer(int socket_id);
void access_flush_socket_memory_one(int socket_id);
void access_socket_memory_hitmealloc(int socket_id);
int load_stored_offsets(int stored_offsets[NUM_CHA][MAX_ADDRESSES], int* valid_entries);
int find_cha_mapped_offset(void* address, int* msr_fds, int num_sockets, cha_event_t* events, int num_events, int *socket_out);
void generate_cha_mapped_offsets(int* msr_fds, int num_sockets, cha_event_t* events, int num_events, int full_scan);

/* Build per-set eviction sets for L1, L2, and LLC from CHA-0 / socket-0 addresses.
 * Scans the socket-0 buffer cache-line by cache-line.  For each line that belongs
 * to CHA-0 on socket-0 it records the address in l1/l2/l3_eviction_sets at the
 * appropriate set index.  Stops early once every set in all three levels has at
 * least <ways> entries (L1_WAYS / L2_WAYS / L3_WAYS), otherwise exhausts the
 * buffer.  Progress is printed to stdout. */
/* fast=0: slow per-line MSR probe (default); fast=1: optimized counter-delta probe. */
void build_eviction_sets(int *msr_fds, int num_sockets, cha_event_t *events, int num_events,
                         int fast);
void flush_targets_and_evsets(void);

/* Save all evset+target physical addresses to EVSET_FILE after a successful build.
 * Returns 1 on success, 0 on failure. */
int save_eviction_sets(void);

/* Try to restore evsets+targets from EVSET_FILE without rebuilding.
 * Allocates extra memory chunks to find pages that are not in the primary buffer.
 * Returns 1 if all sets are fully restored, 0 if rebuild is needed. */
int load_eviction_sets(void);

#endif // SOCKET_MEMORY_H
