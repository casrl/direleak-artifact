// msr_utils.c
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   // For mkdir()
#include <sys/types.h>  // For mode_t
#include <time.h>       // For timestamp
#include <unistd.h>
#include "msr_defs.h"  // Include the header file

int global_file_ctr = 0;

/*
 * find_cpu_sockets — enumerate physical sockets.
 * Intel CHA PMU MSRs are scoped to the physical socket: all CPUs on the same
 * socket share the same uncore counter registers regardless of NUMA domain.
 * On SPR (SNC-2, 4 NUMA domains across 2 sockets) this returns 2.
 * socket_map[socket_id] = (representative_cpu_id + 1).
 */
int find_cpu_sockets(int* socket_map, int max_sockets) {
  int num_sockets = 0;
  DIR* dir = opendir("/sys/devices/system/cpu");
  if (!dir) {
    perror("opendir");
    return -1;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "cpu", 3) == 0) {
      int cpu_id;
      if (sscanf(entry->d_name + 3, "%d", &cpu_id) != 1)
        continue;

      char path[128];
      snprintf(path, sizeof(path),
               "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
               cpu_id);
      FILE* file = fopen(path, "r");
      if (!file) continue;

      int socket_id;
      if (fscanf(file, "%d", &socket_id) != 1) { fclose(file); continue; }
      fclose(file);

      if (socket_map[socket_id] == 0) {
        socket_map[socket_id] = cpu_id + 1;
        num_sockets++;
        if (num_sockets >= max_sockets) break;
      }
    }
  }
  closedir(dir);
  return num_sockets;
}

int open_msr_fds(int* socket_map, int num_sockets, int* msr_fds) {
  int success = 0;  // Assume success

  for (int i = 0; i < num_sockets; i++) {
    if (socket_map[i] > 0) {
      int core_id = socket_map[i] - 1;
      char msr_path[64];
      snprintf(msr_path, sizeof(msr_path), MSR_PATH_FORMAT, core_id);

      int fd = open(msr_path, O_RDWR);
      if (fd < 0) {
        perror("open msr");
        msr_fds[i] = -1;  // Indicate failure
        success = -1;     // Mark as failure
      } else {
        msr_fds[i] = fd;
      }
    }
  }
  return success;
}

void close_msr_fds(int* msr_fds, int num_sockets) {
  for (int i = 0; i < num_sockets; i++) {
    if (msr_fds[i] >= 0) {
      close(msr_fds[i]);
      // printf("Closed MSR file descriptor %d\n", msr_fds[i]);
    }
  }
}

void freeze_counters_global(int* msr_fds, int num_sockets) {
  mfence();
  uint64_t freeze_val = U_MSR_PMON_GLOBAL_CTL_frz_all;
  for (int i = 0; i < num_sockets; i++) {
    if (msr_fds[i] >= 0) {
      if (WRITE_MSR(msr_fds[i], U_MSR_PMON_GLOBAL_CTL, freeze_val) == -1) {
        perror("Error freezing all counters");
        exit(EXIT_FAILURE);
      }
    }
  }
}

void unfreeze_counters_global(int* msr_fds, int num_sockets) {
  mfence();
  uint64_t unfreeze_val = U_MSR_PMON_GLOBAL_CTL_unfrz_all;
  for (int i = 0; i < num_sockets; i++) {
    if (msr_fds[i] >= 0) {
      if (WRITE_MSR(msr_fds[i], U_MSR_PMON_GLOBAL_CTL, unfreeze_val) == -1) {
        perror("Error unfreezing all counters");
        exit(EXIT_FAILURE);
      }
    }
  }
}

// Function to disable all prefetchers by writing the disable mask to
// MSR_PREFETCH_CONTROL.
void disable_prefetch(int* msr_fds, int num_sockets) {
  uint64_t disable_mask =
      MSR_PREFETCH_ALL_DISABLE;  // All prefetchers disabled.
  for (int i = 0; i < num_sockets; i++) {
    if (msr_fds[i] >= 0) {
      if (WRITE_MSR(msr_fds[i], MSR_PREFETCH_CONTROL, disable_mask) == -1) {
        perror("Error disabling prefetch");
        exit(EXIT_FAILURE);
      }
    }
  }
}

void configure_cha_counters(int* msr_fds,
                            int num_sockets,
                            cha_event_t* events,
                            int num_events,
                            char* event_name_list[],
                            int num_events_to_program) {
  if (msr_fds == NULL || events == NULL || event_name_list == NULL) {
    fprintf(stderr, "Error: NULL pointer passed to configure_cha_counters\n");
    return;
  }

  uint64_t msr_val;
  uint64_t msr_num;
  
  for (int i = 0; i < num_sockets; i++) {
    if (msr_fds[i] >= 0) {
      for (int cha = 0; cha < NUM_CHA; cha++) {
        // Step 2.1: RESET all four counters from the UNIT level
        msr_num = CHA_MSR_PMON_BASE(cha);
        msr_val = U_MSR_PMON_UNIT_CTL_rst_both;
        if (WRITE_MSR(msr_fds[i], msr_num, msr_val) == -1) {
          perror("Error writing MSR (reset)");
          continue;  // Go to the next CHA
        }

        // Step 2.2: Setup counters to count events from event_name_list
        for (int j = 0; j < NUM_CTR_PER_CHA; j++) {
          // printf("Event: %s\n", event_name_list[j]);
          if (j >= num_events_to_program)
            break;  // Important: Use num_events_to_program

          unsigned int event_code;
          uint64_t umask, extra = 0;

          // Determine the MSR number based on 'j'
          if (j == 0)
            msr_num = MSR_UNIT_CTRL0(cha);
          else if (j == 1)
            msr_num = MSR_UNIT_CTRL1(cha);
          else if (j == 2)
            msr_num = MSR_UNIT_CTRL2(cha);
          else if (j == 3)
            msr_num = MSR_UNIT_CTRL3(cha);
          else {
            DEBUG_PRINT("Invalid counter number %d\n", j);
            // exit with error
            exit(EXIT_FAILURE);
          }

          if (get_event_code_and_umask(events, num_events, event_name_list[j],
                                       &event_code, &umask) == 0) {
            #if ARCH == 4
              extra = (umask >>
                      8);  // Remove lower 8 bits, keeping the remaining as extra
              umask &= 0xFF;  // Keep only the lower 8 bits
              DEBUG_PRINT("Extra: %llx, Umask: %llx, Event: %x\n",
                          (unsigned long long)extra,
                          (unsigned long long)umask, event_code);
              msr_val = MSR_UNIT_CTL_EXTRA(extra) | MSR_UNIT_CTL_UMASK(umask) |
                        MSR_UNIT_CTL_EVENT(event_code);
            #elif ARCH == 2
              msr_val = MSR_UNIT_CTL_EN | MSR_UNIT_CTL_UMASK(umask) |
                        MSR_UNIT_CTL_EVENT(event_code);
            #elif ARCH == 3
            if (event_code == 0x34) {
              // set bit [57:32] to 0x1BC1
              msr_val = MSR_UNIT_CTL_EN | MSR_UNIT_CTL_UMASK(umask) |
                        MSR_UNIT_CTL_EVENT(event_code) |
                        ((uint64_t)0x1BC1 << 32);
            } else {
              msr_val = MSR_UNIT_CTL_EN | MSR_UNIT_CTL_UMASK(umask) |
                        MSR_UNIT_CTL_EVENT(event_code);
            }
            #else
              DEBUG_PRINT(
                  "Unsupported ARCH. Please define ARCH as SKX, CLX, or SPR.\n");
              exit(EXIT_FAILURE);
            #endif
            if (WRITE_MSR(msr_fds[i], msr_num, msr_val) == -1) {
              printf("Error writing MSR (configure): %lx %lx\n", msr_num,
                     msr_val);
              DEBUG_PRINT("Error writing MSR (configure): %lx %lx\n", msr_num,
                          msr_val);
              exit(EXIT_FAILURE);
            }
            #if ARCH == 2
              // Enable the counter
              // LLC_LOOKUP filter
              msr_num = MSR_UNIT_FILTER0(cha);
              if (event_code == 0x34) {
                // if event code = 0x34, set filter to FMESI
                msr_val = MSR_UNIT_FILTER0_FMESI;
                if (WRITE_MSR(msr_fds[i], msr_num, msr_val) == -1) {
                  perror("Error writing MSR (configure)");
                }
              } else {
                // else, clear the filter
                msr_val = MSR_UNIT_FILTER0_CLR;
                if (WRITE_MSR(msr_fds[i], msr_num, msr_val) == -1) {
                  perror("Error writing MSR (configure)");
                }
              }
            #endif
          } else {
            printf("Event '%s' not found.\n", event_name_list[j]);
          }
        }
      }
    }
  }
}

void read_cha_counters(
    int* msr_fds,
    int num_sockets,
    cha_event_t* events,
    int num_events,
    char* event_name_list[],
    int num_events_to_program,
    int run_idx,
    uint64_t counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][MAX_MONITOR_EVENTS],
    int event_index) {
  if (msr_fds == NULL || event_name_list == NULL) {
    fprintf(stderr, "Error: NULL pointer passed to read_cha_counters\n");
    return;
  }

  uint64_t msr_val;
  uint64_t msr_num;

  mfence();
  for (int i = 0; i < num_sockets; i++) {
    if (msr_fds[i] >= 0) {
      for (int cha = 0; cha < NUM_CHA; cha++) {
        for (int j = 0; j < num_events_to_program; j++) {
          int actual_event_index =
              event_index + j;  // 🔧 Fix: Proper event index tracking

          switch (j) {
            case 0:
              msr_num = MSR_UNIT_CTR0(cha);
              break;
            case 1:
              msr_num = MSR_UNIT_CTR1(cha);
              break;
            case 2:
              msr_num = MSR_UNIT_CTR2(cha);
              break;
            case 3:
              msr_num = MSR_UNIT_CTR3(cha);
              break;
            default:
              perror("MSR number out of range");
              continue;
          }

          if (READ_MSR(msr_fds[i], msr_num, msr_val) == -1) {
            perror("Error reading MSR");
            continue;
          }

          counts[run_idx][i][cha][actual_event_index] = msr_val;
        }
      }
    }
  }
}

void reset_counts_only(int* msr_fds,
                       int num_sockets,
                       int num_events,
                       int num_events_to_program) {
  if (msr_fds == NULL) {
    fprintf(stderr, "Error: NULL pointer passed to configure_cha_counters\n");
    return;
  }

  uint64_t msr_val;
  uint64_t msr_num;

  for (int i = 0; i < num_sockets; i++) {
    if (msr_fds[i] >= 0) {
      for (int cha = 0; cha < NUM_CHA; cha++) {
        // Step 2.1: RESET all four counters from the UNIT level
        msr_num = CHA_MSR_PMON_BASE(cha);
        msr_val = U_MSR_PMON_UNIT_CTL_rst_both;
        if (WRITE_MSR(msr_fds[i], msr_num, msr_val) == -1) {
          perror("Error writing MSR (reset)");
          continue;  // Go to the next CHA
        }
      }
    }
  }
}

void calculate_cha_counters(
    uint64_t old_counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][NUM_CTR_PER_CHA],
    uint64_t new_counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][NUM_CTR_PER_CHA],
    uint64_t counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][NUM_CTR_PER_CHA],
    int num_events_to_program,
    int num_sockets) {
  for (int i = 0; i < NUM_RUNS; i++) {
    for (int j = 0; j < num_sockets; j++) {
      for (int k = 0; k < NUM_CHA; k++) {
        for (int l = 0; l < num_events_to_program; l++) {
          counts[i][j][k][l] = new_counts[i][j][k][l] - old_counts[i][j][k][l];
        }
      }
    }
  }
}

// Function to create nested directories recursively
int create_directory_recursively(const char* path) {
  char temp[512];
  snprintf(temp, sizeof(temp), "%s", path);

  for (char* p = temp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(temp, 0777) && errno != EEXIST) {
        perror("Error creating directory");
        return -1;
      }
      *p = '/';
    }
  }

  if (mkdir(temp, 0777) && errno != EEXIST) {
    perror("Error creating final directory");
    return -1;
  }

  return 0;  // Success
}

void write_event_counts(
    uint64_t counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][MAX_MONITOR_EVENTS],
    int num_events_to_program,
    int num_sockets,
    char** event_name_list,
    const char* benchmark_name) {
  // Get the current timestamp
  time_t now = time(NULL);
  struct tm* t = localtime(&now);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);

  // Ensure the full directory path does not exceed buffer limits
  char dir_path[512];
  snprintf(dir_path, sizeof(dir_path), "output/%s/%s", benchmark_name,
           timestamp);
  if (create_directory_recursively(dir_path) != 0) {
    fprintf(stderr, "Error: Failed to create directories: %s\n", dir_path);
    return;
  }

  // Construct log file path safely
  char filename1[512];
  snprintf(filename1, sizeof(filename1), "%s/%d.log", dir_path,
           global_file_ctr);

  // Ensure "current" directory exists
  char current_dir[512];
  snprintf(current_dir, sizeof(current_dir), "output/current");
  if (create_directory_recursively(current_dir) != 0) {
    fprintf(stderr, "Error: Failed to create 'current' directory.\n");
    return;
  }

  // Construct second log file path
  char filename2[512];
  snprintf(filename2, sizeof(filename2), "output/current/%s.log",
           benchmark_name);

  printf("Writing event counts to %s and %s\n", filename1, filename2);

  FILE* fp1 = fopen(filename1, "w");
  FILE* fp2 = fopen(filename2, "w");
  if (!fp1 || !fp2) {
    perror("Error opening log files");
    if (fp1)
      fclose(fp1);
    if (fp2)
      fclose(fp2);
    return;
  }

  global_file_ctr++;

  // ANSI color codes
  const char* BOLD_GREEN = "\033[1;32m";
  const char* BOLD_CYAN = "\033[1;36m";
  const char* BOLD_WHITE = "\033[1;37m";
  const char* RESET = "\033[0m";

  // Determine maximum event name length
  int max_event_name_len = 0;
  for (int i = 0; i < num_events_to_program; i++) {
    int len = strlen(event_name_list[i]);
    if (len > max_event_name_len) {
      max_event_name_len = len;
    }
  }

// Function to write the same content to both log files
#define LOG(fmt, ...)                 \
  do {                                \
    fprintf(fp1, fmt, ##__VA_ARGS__); \
    fprintf(fp2, fmt, ##__VA_ARGS__); \
  } while (0)

  // Part 1: Overall Average Event Counts with Per-Socket Statistics
  LOG("\n\n%s// Part 1: Overall Average Event Counts%s\n", BOLD_CYAN, RESET);

  // Print the header with dynamic socket count
  LOG("%s%-*s %10s", BOLD_CYAN, max_event_name_len, "Event Name", "Avg");
  for (int socket = 0; socket < num_sockets; socket++) {
    LOG("  %4s-%d / %5s", "S", socket, "Max");
  }
  LOG("%s\n", RESET);

  // Print separator line
  LOG("%s%-*s %10s", BOLD_CYAN, max_event_name_len, "----------", "----------");
  for (int socket = 0; socket < num_sockets; socket++) {
    LOG("  %4s / %4s", "------", "-----");
  }
  LOG("%s\n", RESET);

  for (int event = 0; event < num_events_to_program; event++) {
    double total_event_count = 0.0;
    double per_socket_avg[MAX_SOCKETS] = {0};
    double per_socket_max[MAX_SOCKETS] = {0};

    // Compute event totals, per-socket statistics
    for (int socket = 0; socket < num_sockets; socket++) {
      double socket_total = 0.0;
      double socket_max = 0.0;

      for (int run = 0; run < NUM_RUNS; run++) {
        for (int cha = 0; cha < NUM_CHA; cha++) {
          double event_count = counts[run][socket][cha][event];
          socket_total += event_count;
          if (event_count > socket_max) {
            socket_max = event_count;
          }
          total_event_count += event_count;
        }
      }

      per_socket_avg[socket] = socket_total / (NUM_RUNS * NUM_CHA);
      per_socket_max[socket] = socket_max;
    }

    double avg_event_count =
        total_event_count / (NUM_RUNS * num_sockets * NUM_CHA);

    // Print event name and overall avg
    LOG("%s%-*s%s %s%10.2f", BOLD_GREEN, max_event_name_len,
        event_name_list[event], RESET, BOLD_WHITE, avg_event_count);

    // Print per-socket averages and max values
    for (int socket = 0; socket < num_sockets; socket++) {
      LOG("  %6.0f /%6.0f", per_socket_avg[socket], per_socket_max[socket]);
    }

    LOG("%s\n", RESET);
  }

  // Part 2: Per-CHA Average Event Counts
  LOG("\n\n%s// Part 2: Per-CHA Average Event Counts%s\n", BOLD_CYAN, RESET);

  for (int event = 0; event < num_events_to_program; event++) {
    LOG("\n%sEvent: %s%s\n", BOLD_GREEN, event_name_list[event], RESET);
    LOG("%s%-5s", BOLD_CYAN, "CHA");
    for (int socket = 0; socket < num_sockets; socket++) {
      LOG(" %8s-%d", "Socket", socket);
    }
    LOG("%s\n", RESET);

    LOG("%-5s", "---");
    for (int socket = 0; socket < num_sockets; socket++) {
      LOG(" %10s", "----------");
    }
    LOG("\n");

    for (int cha = 0; cha < NUM_CHA; cha++) {
      LOG("%-5d", cha);
      for (int socket = 0; socket < num_sockets; socket++) {
        double cha_event_count = 0.0;
        for (int run = 0; run < NUM_RUNS; run++) {
          cha_event_count += counts[run][socket][cha][event];
        }
        double avg_cha_event_count = cha_event_count / NUM_RUNS;
        LOG(" %s%10.2f%s", BOLD_WHITE, avg_cha_event_count, RESET);
      }
      LOG("\n");
    }
  }

  // Part 3: Detailed Event Counts with Standard Deviation
  LOG("\n\n%s// Part 3: Detailed Event Counts with Standard Deviation%s\n",
      BOLD_CYAN, RESET);

  for (int event = 0; event < num_events_to_program; event++) {
    LOG("\n%sEvent: %s%s\n", BOLD_GREEN, event_name_list[event], RESET);
    LOG("%s%-5s %-5s", BOLD_CYAN, "Soc", "CHA");
    for (int run = 0; run < NUM_RUNS; run++) {
      LOG(" %8d", run);
    }
    LOG(" %10s %10s%s\n", "Avg", "Std Dev", RESET);

    for (int socket = 0; socket < num_sockets; socket++) {
      for (int cha = 0; cha < NUM_CHA; cha++) {
        double run_counts[NUM_RUNS];
        double sum = 0.0, sum_sq = 0.0;

        LOG("%-5d %-5d", socket, cha);

        for (int run = 0; run < NUM_RUNS; run++) {
          run_counts[run] = counts[run][socket][cha][event];
          sum += run_counts[run];
          sum_sq += run_counts[run] * run_counts[run];
          LOG(" %8.0f", run_counts[run]);
        }

        double avg = sum / NUM_RUNS;
        double variance = (sum_sq / NUM_RUNS) - (avg * avg);
        double stdev = sqrt(variance);

        LOG(" %s%10.2f %10.2f%s\n", BOLD_WHITE, avg, stdev, RESET);
      }
    }
  }

  fclose(fp1);
  fclose(fp2);
}

// Parse a complete hexadecimal event field without truncating SPR UMaskExt bits.
int hex_string_to_uint64(const char* hex_str, uint64_t* value) {
  if (hex_str == NULL || *hex_str == '\0' || value == NULL) {
    return -1;
  }

  errno = 0;
  char* end = NULL;
  unsigned long long parsed = strtoull(hex_str, &end, 0);
  if (errno == ERANGE || end == hex_str || *end != '\0') {
    return -1;
  }

  *value = (uint64_t)parsed;
  return 0;
}

int parse_cha_events(const char* filename,
                     cha_event_t** events,
                     int* num_events) {
  json_error_t error;
  json_t* root = json_load_file(filename, 0, &error);

  if (!root) {
    fprintf(stderr, "Error: %s (line %d, column %d)\n", error.text, error.line,
            error.column);
    return -1;
  }

  if (!json_is_array(root)) {
    fprintf(stderr, "Error: Root is not an array\n");
    json_delete(root);
    return -1;
  }

  // Don't set *num_events yet; we'll determine the *actual* number after
  // filtering

  // TEMPORARY storage for ALL events, including deprecated ones
  cha_event_t* temp_events =
      (cha_event_t*)malloc(json_array_size(root) * sizeof(cha_event_t));
  if (!temp_events) {
    perror("Memory allocation failed");
    json_delete(root);
    return -1;
  }

  int valid_event_count =
      0;  // Keep track of the number of non-deprecated events
  for (int i = 0; i < json_array_size(root); i++) {
    json_t* event_obj = json_array_get(root, i);
    if (!json_is_object(event_obj)) {
      fprintf(stderr, "Error: Event %d is not an object\n", i);
      continue;  // Skip to the next event
    }

    const char* brief_description =
        json_string_value(json_object_get(event_obj, "BriefDescription"));
    // Check for NULL
    if (!brief_description) {
      brief_description = "";
    }
    // Check for deprecation BEFORE allocating or copying other data
    if (strstr(brief_description, "This event is deprecated.") != NULL) {
      continue;  // Skip this event; it's deprecated
    }

    const char* event_code_str =
        json_string_value(json_object_get(event_obj, "EventCode"));
    const char* event_name =
        json_string_value(json_object_get(event_obj, "EventName"));
    json_t* per_pkg_json = json_object_get(event_obj, "PerPkg");
    const char* public_description =
        json_string_value(json_object_get(event_obj, "PublicDescription"));
    const char* umask_str =
        json_string_value(json_object_get(event_obj, "UMask"));
    const char* unit = json_string_value(json_object_get(event_obj, "Unit"));
    const char* filter =
        json_string_value(json_object_get(event_obj, "Filter"));

    // Check for NULL
    if (!event_code_str) {
      event_code_str = "";
    }
    if (!event_name) {
      event_name = "";
    }
    if (!public_description) {
      public_description = "";
    }
    if (!umask_str || *umask_str == '\0') {
      umask_str = "0";
    }
    if (!unit) {
      unit = "";
    }
    if (!filter) {
      filter = "";
    }

    // Copy data to the cha_event_t structure
    strncpy(temp_events[valid_event_count].brief_description, brief_description,
            sizeof(temp_events[valid_event_count].brief_description) - 1);
    temp_events[valid_event_count].brief_description
        [sizeof(temp_events[valid_event_count].brief_description) - 1] = '\0';

    strncpy(temp_events[valid_event_count].event_code_str, event_code_str,
            sizeof(temp_events[valid_event_count].event_code_str) - 1);
    temp_events[valid_event_count]
        .event_code_str[sizeof(temp_events[valid_event_count].event_code_str) -
                        1] = '\0';

    strncpy(temp_events[valid_event_count].event_name, event_name,
            sizeof(temp_events[valid_event_count].event_name) - 1);
    temp_events[valid_event_count]
        .event_name[sizeof(temp_events[valid_event_count].event_name) - 1] =
        '\0';

    temp_events[valid_event_count].per_pkg =
        (per_pkg_json && json_is_string(per_pkg_json) &&
         strcmp(json_string_value(per_pkg_json), "1") == 0)
            ? 1
            : 0;

    strncpy(temp_events[valid_event_count].public_description,
            public_description,
            sizeof(temp_events[valid_event_count].public_description) - 1);
    temp_events[valid_event_count].public_description
        [sizeof(temp_events[valid_event_count].public_description) - 1] = '\0';

    strncpy(temp_events[valid_event_count].umask_str, umask_str,
            sizeof(temp_events[valid_event_count].umask_str) - 1);
    temp_events[valid_event_count]
        .umask_str[sizeof(temp_events[valid_event_count].umask_str) - 1] = '\0';

    strncpy(temp_events[valid_event_count].unit, unit,
            sizeof(temp_events[valid_event_count].unit) - 1);
    temp_events[valid_event_count]
        .unit[sizeof(temp_events[valid_event_count].unit) - 1] = '\0';

    strncpy(temp_events[valid_event_count].filter, filter,
            sizeof(temp_events[valid_event_count].filter) - 1);
    temp_events[valid_event_count]
        .filter[sizeof(temp_events[valid_event_count].filter) - 1] = '\0';

    // Convert complete event fields and reject malformed or oversized codes.
    uint64_t event_code = 0;
    uint64_t umask = 0;
    if (hex_string_to_uint64(
            temp_events[valid_event_count].event_code_str, &event_code) != 0 ||
        event_code > UINT32_MAX ||
        hex_string_to_uint64(
            temp_events[valid_event_count].umask_str, &umask) != 0) {
      fprintf(stderr, "Error: Invalid event encoding for '%s'\n", event_name);
      continue;
    }
    temp_events[valid_event_count].event_code = (unsigned int)event_code;
    temp_events[valid_event_count].umask = umask;

    valid_event_count++;  // Increment the count of valid events
  }

  // Now allocate the CORRECT size for the final events array
  *events = (cha_event_t*)malloc(valid_event_count * sizeof(cha_event_t));
  if (!*events) {
    perror("Memory allocation failed");
    free(temp_events);  // Free the temporary array
    json_delete(root);
    return -1;
  }

  // Copy the valid events from the temporary array to the final array
  memcpy(*events, temp_events, valid_event_count * sizeof(cha_event_t));
  *num_events = valid_event_count;  // Set the correct number of events

  free(temp_events);  // Free the temporary array
  json_delete(root);
  return 0;  // Indicate success
}

void free_cha_events(cha_event_t* events, int num_events) {
  if (events) {
    free(events);
  }
}

void print_event_details(cha_event_t* events,
                         int num_events,
                         const char* event_name_search) {
  if (events == NULL || event_name_search == NULL) {
    return;  // Handle invalid input
  }

  int found = 0;  // Flag to check if any events were found
  for (int i = 0; i < num_events; i++) {
    if (strcmp(events[i].event_name, event_name_search) == 0) {
      found = 1;
      printf("Event Name: %s\n", events[i].event_name);
      printf("  Brief Description: %s\n", events[i].brief_description);
      printf("  Public Description: %s\n", events[i].public_description);
      printf("  Event Code: 0x%x\n", events[i].event_code);
      printf("  UMask: 0x%llx\n", (unsigned long long)events[i].umask);
      printf("  Unit: %s\n", events[i].unit);
      printf("  Filter: %s\n", events[i].filter);
      printf("  PerPkg: %d\n", events[i].per_pkg);
      printf("  ---\n");
    }
  }
  if (!found) {
    printf("Event '%s' not found.\n", event_name_search);
  }
}

void print_events_by_code(cha_event_t* events,
                          int num_events,
                          unsigned int event_code_search) {
  if (events == NULL) {
    return;  // Handle invalid input
  }

  int found = 0;
  for (int i = 0; i < num_events; i++) {
    if (events[i].event_code == event_code_search) {
      found = 1;
      printf("Event Name: %s\n", events[i].event_name);
      printf("  Brief Description: %s\n", events[i].brief_description);
      printf("  Public Description: %s\n", events[i].public_description);
      printf("  Event Code: 0x%x\n", events[i].event_code);
      printf("  UMask: 0x%llx\n", (unsigned long long)events[i].umask);
      printf("  Unit: %s\n", events[i].unit);
      printf("  Filter: %s\n", events[i].filter);
      printf("  PerPkg: %d\n", events[i].per_pkg);
      printf("  ---\n");
    }
  }
  if (!found) {
    printf("Event with code '0x%x' not found.\n", event_code_search);
  }
}

int get_event_code_and_umask(cha_event_t* events,
                             int num_events,
                             const char* event_name,
                             unsigned int* event_code,
                             uint64_t* umask) {
  if (events == NULL || event_name == NULL || event_code == NULL ||
      umask == NULL) {
    return -1;  // Invalid input
  }

  for (int i = 0; i < num_events; i++) {
    if (strcmp(events[i].event_name, event_name) == 0) {
      *event_code = events[i].event_code;
      *umask = events[i].umask;
      return 0;  // Success
    }
  }

  return -1;  // Event not found
}

// Helper function to convert a string to uppercase
void str_to_upper(char* str) {
  if (str == NULL) {
    return;
  }
  for (int i = 0; str[i]; i++) {
    str[i] = toupper((unsigned char)str[i]);
  }
}

// Helper function to wrap text to a specified width
void wrap_text(const char* text, int width) {
  int len = strlen(text);
  int start = 0;
  int end = 0;

  while (start < len) {
    // Find the end of the current line
    end = start + width;
    if (end > len) {
      end = len;
    } else {
      // Backtrack to the last space within the width limit
      while (end > start && text[end] != ' ' && text[end] != '\t' &&
             text[end] != '\n') {
        end--;
      }
      // If no space found, just break at the width limit
      if (end == start) {
        end = start + width;
      }
    }

    // Print the line segment
    printf("%.*s\n", end - start, text + start);

    // Update the start position for the next line
    start = end;
    // Skip any spaces at the beginning of the next line
    while (start < len && (text[start] == ' ' || text[start] == '\t')) {
      start++;
    }
    // Indent subsequent lines
    if (start < len) {
      printf("%-16s",
             "");  // 16 spaces for alignment (length of "Event Name:     ")
    }
  }
}

void print_events_by_filter(cha_event_t* events, int num_events) {
  if (events == NULL) {
    printf("No events loaded.\n");
    return;
  }

  char filter[256];  // Buffer to store user input.
  printf("Enter search filter: ");
  if (fgets(filter, sizeof(filter), stdin) == NULL) {
    printf("Error reading input.\n");
    return;
  }
  filter[strcspn(filter, "\n")] = '\0';  // Remove trailing newline
  str_to_upper(filter);

  // First pass: determine the maximum length of matching event names
  int max_width = (int)strlen("Event Name");
  int match_found = 0;
  for (int i = 0; i < num_events; i++) {
    char upper_event_name[sizeof(events[i].event_name)];
    char upper_brief_description[sizeof(events[i].brief_description)];
    strcpy(upper_event_name, events[i].event_name);
    str_to_upper(upper_event_name);
    strcpy(upper_brief_description, events[i].brief_description);
    str_to_upper(upper_brief_description);

    if (strstr(upper_event_name, filter) != NULL ||
        strstr(upper_brief_description, filter) != NULL) {
      match_found = 1;
      int len = (int)strlen(events[i].event_name);
      if (len > max_width) {
        max_width = len;
      }
    }
  }
  if (!match_found) {
    printf("No events found matching the filter '%s'.\n", filter);
    return;
  }

  // Print header using dynamic width for "Event Name" column
  printf("\nMatching events:\n");
  for (int i = 0; i < max_width; i++)
    printf("-");
  printf("-+------------+----------+------------------\n");

  printf("%-*s | %-10s | %-8s | %-16s\n", max_width, "Event Name", "Event Code",
         "UMask", "MSR Value");

  // Print header separator line
  for (int i = 0; i < max_width; i++)
    printf("-");
  printf("-+------------+----------+------------------\n");

  // Second pass: print matching events with full event names
  for (int i = 0; i < num_events; i++) {
    char upper_event_name[sizeof(events[i].event_name)];
    char upper_brief_description[sizeof(events[i].brief_description)];
    strcpy(upper_event_name, events[i].event_name);
    str_to_upper(upper_event_name);
    strcpy(upper_brief_description, events[i].brief_description);
    str_to_upper(upper_brief_description);

    if (strstr(upper_event_name, filter) != NULL ||
        strstr(upper_brief_description, filter) != NULL) {
      uint64_t msr_val;
#if ARCH == 4
      msr_val = MSR_UNIT_CTL_EXTRA(events[i].umask >> 8) |
                MSR_UNIT_CTL_UMASK(events[i].umask & 0xFF) |
                MSR_UNIT_CTL_EVENT(events[i].event_code);
#else
      msr_val = MSR_UNIT_CTL_EN | MSR_UNIT_CTL_UMASK(events[i].umask) |
                MSR_UNIT_CTL_EVENT(events[i].event_code);
#endif
      printf("%-*s | 0x%-8x | 0x%-10llx | 0x%-14lx\n", max_width,
             events[i].event_name, events[i].event_code,
             (unsigned long long)events[i].umask, msr_val);
      for (int j = 0; j < max_width; j++)
        printf("-");
      printf("-+------------+----------+------------------\n");
    }
  }
}

void select_cha_events(cha_event_t* events,
                       int num_events,
                       char*** event_name_list,
                       int* num_events_to_program) {
  // Interactive mode: allow user to search multiple times before selecting
  // events.
  char choice[10];
  printf("Interactive mode: Filter available events.\n");
  do {
    print_events_by_filter(events, num_events);
    printf("\nDo you want to search again? (y/n): ");
    if (fgets(choice, sizeof(choice), stdin) != NULL) {
      choice[strcspn(choice, "\n")] = '\0';  // Remove newline
    } else {
      break;
    }
  } while (choice[0] == 'y' || choice[0] == 'Y');

  printf("\nEnter number of events to track (max %d): ", NUM_CTR_PER_CHA);
  int n;
  if (scanf("%d", &n) != 1 || n < 1 || n > NUM_CTR_PER_CHA) {
    printf("Invalid number of events. Using default event.\n");
    n = 1;
  }

  // Clear input buffer.
  int ch;
  while ((ch = getchar()) != '\n' && ch != EOF)
    ;

  *num_events_to_program = n;
  *event_name_list = malloc(n * sizeof(char*));

  for (int i = 0; i < n; i++) {
    char buffer[128];
    printf("Enter event name %d: ", i + 1);
    if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
      buffer[strcspn(buffer, "\n")] = '\0';
      (*event_name_list)[i] = strdup(buffer);
    } else {
      printf("Error reading input, using default event.\n");
      (*event_name_list)[i] = strdup("UNC_CHA_HITME_HIT.EX_RDS");
    }
  }
}
