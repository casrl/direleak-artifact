// msr_defs.h
#ifndef MSR_DEFS_H
#define MSR_DEFS_H

#include <jansson.h>  // Include for JSON
#include <stdint.h>   // Include for uint64_t
#include <unistd.h>   // For pread() and pwrite()
#include "arch.h"
#include "util.h"

#define NUM_RUNS 10
#define MAX_MONITOR_EVENTS 10  // Maximum number of counters to monitor
#define MAX_ADDRESSES 10000

// Prefetch Control MSR
#define MSR_PREFETCH_CONTROL 0x1A4
#define MSR_PREFETCH_HW_DISABLE 0
#define MSR_PREFETCH_CL_DISABLE 1
#define MSR_PREFETCH_DCU_DISABLE 2
#define MSR_PREFETCH_IP_DISABLE 3
#define MSR_PREFETCH_ALL_DISABLE                                 \
  (1 << MSR_PREFETCH_HW_DISABLE | 1 << MSR_PREFETCH_CL_DISABLE | \
   1 << MSR_PREFETCH_DCU_DISABLE | 1 << MSR_PREFETCH_IP_DISABLE)

#define MSR_PATH_FORMAT \
  "/dev/cpu/%d/msr"  // Format string for the path to the MSR file

// Helper functions to read and write MSRs (Prototypes)
#define READ_MSR(msr_fd, offset, value) \
  pread(msr_fd, &value, sizeof(value), offset)
#define WRITE_MSR(msr_fd, offset, value) \
  pwrite(msr_fd, &value, sizeof(value), offset)

// Parse cha_events.json file
typedef struct {
  char brief_description[256];  // Adjust size as needed
  char event_code_str[8];       // Store as string initially
  char event_name[64];          // Adjust size as needed
  int per_pkg;
  char public_description[512];  // Adjust size as needed
  char umask_str[24];            // Store as string initially. Must hold extended
                                 // umasks like "0xc817fd01"/"0x1fffff"; [8] truncated
                                 // them before parse, corrupting the umask.
  char unit[32];                 // Adjust size as needed
  char filter[64];  // For filters such as "Filter": "config1=0x40040e33"
  unsigned int event_code;  // Converted event code
  uint64_t umask;           // Converted umask, including SPR UMaskExt bits
} cha_event_t;

int find_cpu_sockets(int* socket_map, int max_sockets);
int open_msr_fds(int* socket_map, int num_sockets, int* msr_fds);
void close_msr_fds(int* msr_fds, int num_sockets);
void freeze_counters_global(int* msr_fds, int num_sockets);
void unfreeze_counters_global(int* msr_fds, int num_sockets);
void configure_cha_counters(int* msr_fds,
                            int num_sockets,
                            cha_event_t* events,
                            int num_events,
                            char* event_name_list[],
                            int num_events_to_program);
void read_cha_counters(
    int* msr_fds,
    int num_sockets,
    cha_event_t* events,
    int num_events,
    char* event_name_list[],
    int num_events_to_program,
    int run_idx,
    uint64_t counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][MAX_MONITOR_EVENTS],
  int event_index);
void calculate_cha_counters(
    uint64_t old_counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][NUM_CTR_PER_CHA],
    uint64_t new_counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][NUM_CTR_PER_CHA],
    uint64_t counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][NUM_CTR_PER_CHA],
    int num_events_to_program,
    int num_sockets);
void write_event_counts(
    uint64_t counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][MAX_MONITOR_EVENTS],
    int num_events_to_program,
    int num_sockets,
    char** event_name_list,
    const char* benchmark_name);

// Function Prototypes
int parse_cha_events(const char* filename,
                     cha_event_t** events,
                     int* num_events);
void free_cha_events(cha_event_t* events, int num_events);
int hex_string_to_uint64(const char* hex_str, uint64_t* value);
void print_event_details(cha_event_t* events,
                         int num_events,
                         const char* event_name_search);
void print_events_by_code(cha_event_t* events,
                          int num_events,
                          unsigned int event_code_search);
int get_event_code_and_umask(cha_event_t* events,
                             int num_events,
                             const char* event_name,
                             unsigned int* event_code,
                             uint64_t* umask);
void str_to_upper(char* str);
void print_events_by_filter(cha_event_t* events, int num_events);
void select_cha_events(cha_event_t *events, int num_events, 
    char ***event_name_list, int *num_events_to_program);

void disable_prefetch(int* msr_fds, int num_sockets);

#endif  // MSR_DEFS_H
