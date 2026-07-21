#ifndef ARCH_DETAIL_SAPPHIRERAPIDS_H
#define ARCH_DETAIL_SAPPHIRERAPIDS_H

#include <stdint.h>

#ifndef ARCH
#define ARCH 4
#endif

#define MAX_SOCKETS 4
// Determine the number of CHA:
// lspci | grep :1e.3
// setpci -s XX:1e.3 0x9c.l
// XX in based on the lspci output
#define NUM_CHA 56   /* 28 CHAs per NUMA domain × 2 domains per socket (SNC-2) */
#define NUM_CTR_PER_CHA 4

// Cache geometry (Sapphire Rapids)
#define L1_WAYS         12
#define L1_SETS         64
#define L2_WAYS         16
#define L2_SETS         2048
#define L3_WAYS         15
#define L3_SETS         2048    /* Per-slice. 56 CHAs/socket (SNC-2: 28/cluster);
                                 * 114688 socket sets / 56 slices = 2048. PA bit 17
                                 * is a slice-hash bit, NOT an in-slice set bit
                                 * (hardware-verified by L3 associativity test). */

// log2(cache-line size): cache-line = 64 bytes = 2^6
#define CACHE_LINE_BITS 6

// Set-index masks: zero the block-offset bits, keep only the set-index bits
//   L1: bits[11:6]   (64  sets)
//   L2: bits[16:6]   (2048 sets)
//   L3: bits[16:6]   (2048 sets, per-slice — same index bits as L2)
#define L1_MASK  ((uintptr_t)(L1_SETS - 1) << CACHE_LINE_BITS)
#define L2_MASK  ((uintptr_t)(L2_SETS - 1) << CACHE_LINE_BITS)
#define L3_MASK  ((uintptr_t)(L3_SETS - 1) << CACHE_LINE_BITS)

// Extract set index from an address
#define L1_SET(addr)  (((uintptr_t)(addr) & L1_MASK) >> CACHE_LINE_BITS)
#define L2_SET(addr)  (((uintptr_t)(addr) & L2_MASK) >> CACHE_LINE_BITS)
#define L3_SET(addr)  (((uintptr_t)(addr) & L3_MASK) >> CACHE_LINE_BITS)


#define JSON_FILE_PATH "events/cha_events_spr_parsed.json" // Path to events json file
#define OFFSET_FILE "cha_map_spr.log"
#define EVSET_FILE  "evset/spr_evset.bin"

// Global Performance Monitoring Control MSRs
#define U_MSR_PMON_GLOBAL_CTL          0x2FF0L       // contains bits that can stop (.frz_all) / restart (.unfrz_all) all the uncore counters
#define U_MSR_PMON_GLOBAL_STATUS       0x2FF2L       // 0x2FF2,0x2FF3 Global Status
#define U_MSR_PMON_GLOBAL_CTL_frz_all  (1UL << 0)  // Freeze all uncore performance monitors (bit 0 to 1)
#define U_MSR_PMON_GLOBAL_CTL_unfrz_all (0UL << 0)          // unfreeze all counters (bit 0 to 0)

// Unit Level PMON State
#define CHA_MSR_PMON_BASE(cha)   (0x2000 + (cha) * 0x10)     // Unit Ctrl = 0x2000 + (CHA * 0x10)
#define CHA_MSR_PMON_STATUS(cha) (0x2001 + (cha) * 0x10)     // Unit Status = 0x2001 + (CHA * 0x10)

// Unit Level PMON State RESET
#define U_MSR_PMON_UNIT_CTL_rst_ctrl (1UL << 8)
#define U_MSR_PMON_UNIT_CTL_rst_ctrs (1UL << 9)
#define U_MSR_PMON_UNIT_CTL_rst_both (U_MSR_PMON_UNIT_CTL_rst_ctrl | U_MSR_PMON_UNIT_CTL_rst_ctrs)

// Unit PMON state - Counter/Control Pairs
// Ctrl0 is at 0x2, ctrl1 is at 0x3, ctrl2 is at 0x4, ctrl3 is at 0x5 from the specific CHA_MSR_PMON_BASE
#define MSR_UNIT_CTRL0(cha)   (CHA_MSR_PMON_BASE(cha) + 0x2)
#define MSR_UNIT_CTRL1(cha)   (CHA_MSR_PMON_BASE(cha) + 0x3)
#define MSR_UNIT_CTRL2(cha)   (CHA_MSR_PMON_BASE(cha) + 0x4)
#define MSR_UNIT_CTRL3(cha)   (CHA_MSR_PMON_BASE(cha) + 0x5)
// Ctr0 is at 0x8, ctr1 is at 0x9, ctr2 is at 0xA, ctr3 is at 0xB from the specific CHA_MSR_PMON_BASE
#define MSR_UNIT_CTR0(cha)    (CHA_MSR_PMON_BASE(cha) + 0x8)
#define MSR_UNIT_CTR1(cha)    (CHA_MSR_PMON_BASE(cha) + 0x9)
#define MSR_UNIT_CTR2(cha)    (CHA_MSR_PMON_BASE(cha) + 0xA)
#define MSR_UNIT_CTR3(cha)    (CHA_MSR_PMON_BASE(cha) + 0xB)

// CHA Filters
#define MSR_UNIT_FILTER0(cha)   (CHA_MSR_PMON_BASE(cha) + 0xE)
#define MSR_UNIT_FILTER1(cha)   (CHA_MSR_PMON_BASE(cha) + 0x6) // Not available
// Filter0 FMESI filter
#define MSR_UNIT_FILTER0_FMESI 0x01E20000
#define MSR_UNIT_FILTER0_CLR 0x200

// Unit PMON state - Control reset
#define MSR_UNIT_CTL_RST    (1UL << 17)
// Unit PMON state - Control enable
// Sapphire Rapids does not have a unit counter ctrl enable bit
#define MSR_UNIT_CTL_EN     (0UL << 0)
// Unit PMON state - Control umask bits (8 bits [15:8])
#define MSR_UNIT_CTL_UMASK(umask)   ((umask) << 8)
// Unit PMON state - Control event bits (8 bits [7:0])
#define MSR_UNIT_CTL_EVENT(event)   (event)
// Unit PMON state - Control umask extra bits (UMaskExt, bits [57:32])
// Cast to uint64_t first: `extra` is a 32-bit unsigned int, so `(extra) << 32`
// is undefined behaviour (x86 masks the shift count to 0), which would drop the
// extension into the low bits and corrupt the event/umask.
#define MSR_UNIT_CTL_EXTRA(extra)   ((uint64_t)(extra) << 32)

#endif // ARCH_DETAIL_SAPPHIRERAPIDS_H
