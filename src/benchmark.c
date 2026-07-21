#include "benchmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#define BENCHMARK_DIR "bin/"
#define MAX_BENCHMARKS 100
#define MAX_PATH_LEN 512

Benchmark *benchmarks[MAX_BENCHMARKS];
int num_benchmarks = 0;

/* Parallel tracking arrays for hot-reload support */
static void  *handles[MAX_BENCHMARKS];
static char   so_paths[MAX_BENCHMARKS][MAX_PATH_LEN];
static time_t so_mtimes[MAX_BENCHMARKS];

// Check if a benchmark with the same name already exists; return its index or -1
static int find_by_name(const char *name) {
    for (int i = 0; i < num_benchmarks; i++) {
        if (strcmp(benchmarks[i]->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// Check if a benchmark with the same name already exists
int is_duplicate(const char *name) {
    return find_by_name(name) >= 0;
}

/* Load a single .so file and append it to the benchmarks array.
 * Returns 1 on success, 0 on failure. */
static int load_one_so(const char *so_path) {
    void *handle = dlopen(so_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "Failed to load %s: %s\n", so_path, dlerror());
        return 0;
    }

    Benchmark *benchmark = (Benchmark *) dlsym(handle, "benchmark");
    if (!benchmark) {
        fprintf(stderr, "Failed to find 'benchmark' symbol in %s\n", so_path);
        dlclose(handle);
        return 0;
    }

    if (is_duplicate(benchmark->name)) {
        fprintf(stderr, "Error: Duplicate benchmark name '%s' found in %s\n", benchmark->name, so_path);
        dlclose(handle);
        return 0;
    }

    struct stat st;
    time_t mtime = 0;
    if (stat(so_path, &st) == 0)
        mtime = st.st_mtime;

    int idx = num_benchmarks++;
    benchmarks[idx] = benchmark;
    handles[idx]    = handle;
    strncpy(so_paths[idx], so_path, MAX_PATH_LEN - 1);
    so_paths[idx][MAX_PATH_LEN - 1] = '\0';
    so_mtimes[idx]  = mtime;
    return 1;
}

/* Sort benchmarks[], handles[], so_paths[], so_mtimes[] together by name. */
static void sort_benchmarks(void) {
    for (int i = 1; i < num_benchmarks; i++) {
        Benchmark *kb = benchmarks[i];
        void      *kh = handles[i];
        char       kp[MAX_PATH_LEN];
        time_t     km = so_mtimes[i];
        strncpy(kp, so_paths[i], MAX_PATH_LEN);

        int j = i - 1;
        while (j >= 0 && strcmp(benchmarks[j]->name, kb->name) > 0) {
            benchmarks[j + 1] = benchmarks[j];
            handles[j + 1]    = handles[j];
            memcpy(so_paths[j + 1], so_paths[j], MAX_PATH_LEN);
            so_mtimes[j + 1]  = so_mtimes[j];
            j--;
        }
        benchmarks[j + 1] = kb;
        handles[j + 1]    = kh;
        memcpy(so_paths[j + 1], kp, MAX_PATH_LEN);
        so_mtimes[j + 1]  = km;
    }
}

// Function to dynamically load benchmarks from shared object files (*.so)
void load_benchmarks() {
    DIR *dir = opendir(BENCHMARK_DIR);
    if (!dir) {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strstr(entry->d_name, ".so")) continue;

        char so_path[MAX_PATH_LEN];
        snprintf(so_path, sizeof(so_path), "%s%s", BENCHMARK_DIR, entry->d_name);

        if (num_benchmarks >= MAX_BENCHMARKS) break;
        load_one_so(so_path);
    }

    closedir(dir);
    sort_benchmarks();
}

/* Scan bin/ for .so files; reload changed ones and pick up new ones.
 * Returns 1 if any change was made (triggers menu redraw), 0 otherwise. */
int sync_benchmarks(void) {
    int changed = 0;

    DIR *dir = opendir(BENCHMARK_DIR);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strstr(entry->d_name, ".so")) continue;

        char so_path[MAX_PATH_LEN];
        snprintf(so_path, sizeof(so_path), "%s%s", BENCHMARK_DIR, entry->d_name);

        struct stat st;
        if (stat(so_path, &st) != 0) continue;
        time_t mtime = st.st_mtime;

        /* Check if this .so is already loaded (by path) */
        int found_idx = -1;
        for (int i = 0; i < num_benchmarks; i++) {
            if (strcmp(so_paths[i], so_path) == 0) {
                found_idx = i;
                break;
            }
        }

        if (found_idx >= 0) {
            /* Already loaded — check if mtime changed */
            if (mtime != so_mtimes[found_idx]) {
                /* Reload */
                dlclose(handles[found_idx]);

                void *new_handle = dlopen(so_path, RTLD_NOW);
                if (!new_handle) {
                    fprintf(stderr, "Reload failed for %s: %s\n", so_path, dlerror());
                    continue;
                }

                Benchmark *new_bm = (Benchmark *) dlsym(new_handle, "benchmark");
                if (!new_bm) {
                    fprintf(stderr, "No 'benchmark' symbol in reloaded %s\n", so_path);
                    dlclose(new_handle);
                    continue;
                }

                benchmarks[found_idx] = new_bm;
                handles[found_idx]    = new_handle;
                so_mtimes[found_idx]  = mtime;
                printf("Reloaded: %s\n", new_bm->name);
                changed = 1;
            }
        } else {
            /* New .so — load it */
            if (num_benchmarks >= MAX_BENCHMARKS) continue;
            int old_count = num_benchmarks;
            if (load_one_so(so_path)) {
                printf("Loaded new benchmark: %s\n", benchmarks[old_count]->name);
                sort_benchmarks();
                changed = 1;
            }
        }
    }

    closedir(dir);
    return changed;
}

/* dlclose all loaded benchmark handles */
void unload_all_benchmarks(void) {
    for (int i = 0; i < num_benchmarks; i++) {
        if (handles[i]) {
            dlclose(handles[i]);
            handles[i] = NULL;
        }
    }
}

// Get a benchmark by name
Benchmark* get_benchmark_by_name(const char *name) {
    int idx = find_by_name(name);
    return (idx >= 0) ? benchmarks[idx] : NULL;
}

// Get the index of a benchmark by name (-1 if not found)
int get_benchmark_index(const char *name) {
    return find_by_name(name);
}

// List all available benchmarks
void list_available_benchmarks() {
    printf("Available Benchmarks:\n");
    for (int i = 0; i < num_benchmarks; i++) {
        printf("  - %s\n", benchmarks[i]->name);
    }
}
