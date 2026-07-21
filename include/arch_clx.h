#ifndef ARCH_DETAIL_CASCADELAKEX_H
#define ARCH_DETAIL_CASCADELAKEX_H

#include <stdint.h>

#ifndef ARCH
#define ARCH 2
#endif

#define MAX_SOCKETS 4
#define NUM_CHA 28
#define NUM_CTR_PER_CHA 4

#define JSON_FILE_PATH "events/cha_events_clx_parsed.json" // Path to events json file
#define OFFSET_FILE "cha_map_clx_mammoth.log"
#define EVSET_FILE  "evset/clx_evset.bin"

// Global Performance Monitoring Control MSRs
#define U_MSR_PMON_GLOBAL_CTL          0x0700L       // contains bits that can stop (.frz_all) / restart (.unfrz_all) all the uncore counters
#define U_MSR_PMON_GLOBAL_STATUS       0x0701L       // shows overflows
#define U_MSR_PMON_GLOBAL_CTL_frz_all  (1UL << 63)           // freeze all counters (bit 63 to 1)
#define U_MSR_PMON_GLOBAL_CTL_unfrz_all (1UL << 61)          // unfreeze all counters (bit 61 to 1)

// Unit Level PMON State
#define CHA_MSR_PMON_BASE(cha)   (0x0E00 + (cha) * 0x10)     // Unit Ctrl = 0x0E00 + (CHA * 0x10)
#define CHA_MSR_PMON_STATUS(cha) (0x0E07 + (cha) * 0x10)     // Unit Status = 0x0E07 + (CHA * 0x10)

// Unit Level PMON State RESET
#define U_MSR_PMON_UNIT_CTL_rst_ctrl (1UL << 0)
#define U_MSR_PMON_UNIT_CTL_rst_ctrs (1UL << 1)
#define U_MSR_PMON_UNIT_CTL_rst_both (U_MSR_PMON_UNIT_CTL_rst_ctrl | U_MSR_PMON_UNIT_CTL_rst_ctrs)

// Unit PMON state - Counter/Control Pairs
// Ctrl0 is at 0x1, ctrl1 is at 0x2, ctrl2 is at 0x3, ctrl3 is at 0x4 from the specific CHA_MSR_PMON_BASE
#define MSR_UNIT_CTRL0(cha)   (CHA_MSR_PMON_BASE(cha) + 0x1)
#define MSR_UNIT_CTRL1(cha)   (CHA_MSR_PMON_BASE(cha) + 0x2)
#define MSR_UNIT_CTRL2(cha)   (CHA_MSR_PMON_BASE(cha) + 0x3)
#define MSR_UNIT_CTRL3(cha)   (CHA_MSR_PMON_BASE(cha) + 0x4)
// Ctr0 is at 0x8, ctr1 is at 0x9, ctr2 is at 0xA, ctr3 is at 0xB from the specific CHA_MSR_PMON_BASE
#define MSR_UNIT_CTR0(cha)    (CHA_MSR_PMON_BASE(cha) + 0x8)
#define MSR_UNIT_CTR1(cha)    (CHA_MSR_PMON_BASE(cha) + 0x9)
#define MSR_UNIT_CTR2(cha)    (CHA_MSR_PMON_BASE(cha) + 0xA)
#define MSR_UNIT_CTR3(cha)    (CHA_MSR_PMON_BASE(cha) + 0xB)

// CHA Filters
#define MSR_UNIT_FILTER0(cha)   (CHA_MSR_PMON_BASE(cha) + 0x5)
#define MSR_UNIT_FILTER1(cha)   (CHA_MSR_PMON_BASE(cha) + 0x6)
// Filter0 FMESI filter
#define MSR_UNIT_FILTER0_FMESI 0x01E20000
#define MSR_UNIT_FILTER0_CLR 0x200

// Unit PMON state - Control reset
#define MSR_UNIT_CTL_RST    (1UL << 17)
// Unit PMON state - Control enable
#define MSR_UNIT_CTL_EN     (1UL << 22)
// Unit PMON state - Control umask bits (8 bits [15:7])
#define MSR_UNIT_CTL_UMASK(umask)   ((umask) << 8)
// Unit PMON state - Control event bits (8 bits [7:0])
#define MSR_UNIT_CTL_EVENT(event)   (event)


// Corrected Cache Configurations
#define L1_SIZE (32 * 1024)           // 32 KB
#define L2_SIZE (1 * 1024 * 1024)     // 1 MB
#define L3_SIZE ((38 * 1024 * 1024) + (512 * 1024))  // 38.5 MB
// Corrected L3 Slice Size Calculation
#define L3_SLICE_SIZE (L3_SIZE / NUM_CHA)

#define L1_WAYS         8
#define L1_SETS         64
#define L2_WAYS         16
#define L2_SETS         1024
#define L3_WAYS         11
#define L3_SETS         2048    /* Per-slice */

// log2(cache-line size): cache-line = 64 bytes = 2^6
#define CACHE_LINE_BITS 6

// Set-index masks: zero the block-offset bits, keep only the set-index bits
//   L1: bits[11:6]   (64  sets)
//   L2: bits[15:6]   (1024 sets)
//   L3: bits[16:6]   (2048 sets)
#define L1_MASK  ((uintptr_t)(L1_SETS - 1) << CACHE_LINE_BITS)
#define L2_MASK  ((uintptr_t)(L2_SETS - 1) << CACHE_LINE_BITS)
#define L3_MASK  ((uintptr_t)(L3_SETS - 1) << CACHE_LINE_BITS)

// Extract set index from an address
#define L1_SET(addr)  (((uintptr_t)(addr) & L1_MASK) >> CACHE_LINE_BITS)
#define L2_SET(addr)  (((uintptr_t)(addr) & L2_MASK) >> CACHE_LINE_BITS)
#define L3_SET(addr)  (((uintptr_t)(addr) & L3_MASK) >> CACHE_LINE_BITS)

#define TARGET_SET_MASK 0x1B // Targeting set_id 27 (11001)

#endif // ARCH_DETAIL_CASCADELAKEX_H
