#include <numa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include "benchmark.h"
#include "msr_defs.h"
#include "socket_memory.h"
#include "util.h"

uint64_t new_counts[NUM_RUNS][MAX_SOCKETS][NUM_CHA][MAX_MONITOR_EVENTS] = {0};

int load_monitor_counters(char*** event_name_list, int* num_events_to_monitor);

/* ── terminal state saved for cleanup on SIGINT ──────────────────────────── */
static struct termios g_saved_termios;
static int            g_termios_saved = 0;

static void restore_terminal(void) {
    if (g_termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
}

static void sigint_handler(int sig) {
    (void)sig;
    restore_terminal();
    unload_all_benchmarks();
    _exit(EXIT_SUCCESS);
}

/* ── Interactive TUI menu ────────────────────────────────────────────────── */
/*
 * Returns the index of the selected benchmark, or -1 if the user quits.
 * current_idx is used to pre-highlight the current benchmark.
 */
static int select_benchmark_interactive(int current_idx) {
    if (num_benchmarks == 0) {
        fprintf(stderr, "No benchmarks loaded.\n");
        return -1;
    }

    /* Clamp initial selection */
    if (current_idx < 0) current_idx = 0;
    if (current_idx >= num_benchmarks) current_idx = num_benchmarks - 1;

    int sel = current_idx;

    /* Save terminal state and switch to raw, no-echo mode */
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_saved_termios);
    g_termios_saved = 1;
    raw = g_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    /* First draw — print menu lines (we'll redraw in-place after that) */
    int menu_lines = num_benchmarks + 3; /* 2 separator lines + header + entries */

    /* Print initial menu */
    printf("\033[1m─────────────────────────────────────────────\033[0m\n");
    printf("Select benchmark (\033[1m↑↓\033[0m navigate, \033[1mEnter\033[0m run, \033[1mq\033[0m quit):\n");
    for (int i = 0; i < num_benchmarks; i++) {
        const char *desc = benchmarks[i]->description ? benchmarks[i]->description : "";
        if (i == sel)
            printf("\033[1;36m> %d. %s: %s\033[0m\n", i + 1, benchmarks[i]->name, desc);
        else
            printf("  %d. %s: %s\n", i + 1, benchmarks[i]->name, desc);
    }
    printf("\033[1m─────────────────────────────────────────────\033[0m\n");
    fflush(stdout);

    int confirmed = 0;
    while (!confirmed) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        if (c == 'q' || c == 'Q') {
            sel = -1;
            break;
        } else if (c == '\r' || c == '\n') {
            confirmed = 1;
            break;
        } else if (c == '\x1b') {
            /* Escape sequence: read two more bytes */
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) break;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) break;
            if (seq[0] == '[') {
                if (seq[1] == 'A') {           /* Up arrow */
                    if (sel > 0) sel--;
                } else if (seq[1] == 'B') {    /* Down arrow */
                    if (sel < num_benchmarks - 1) sel++;
                }
            }
        }

        /* Redraw in-place: move cursor up menu_lines lines */
        printf("\033[%dA", menu_lines);
        printf("\033[1m─────────────────────────────────────────────\033[0m\n");
        printf("Select benchmark (\033[1m↑↓\033[0m navigate, \033[1mEnter\033[0m run, \033[1mq\033[0m quit):\n");
        for (int i = 0; i < num_benchmarks; i++) {
            const char *desc = benchmarks[i]->description ? benchmarks[i]->description : "";
            if (i == sel)
                printf("\033[1;36m> %d. %s: %s\033[0m\n", i + 1, benchmarks[i]->name, desc);
            else
                printf("  %d. %s: %s\n", i + 1, benchmarks[i]->name, desc);
        }
        printf("\033[1m─────────────────────────────────────────────\033[0m\n");
        fflush(stdout);
    }

    /* Restore terminal */
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    g_termios_saved = 0;

    return sel;
}

/* ── Benchmark execution session ─────────────────────────────────────────── */
static void run_benchmark_session(
    Benchmark *benchmark,
    BenchmarkCtx *ctx,
    int *msr_fds, int num_sockets,
    cha_event_t *events, int num_events,
    char **event_name_list, int num_total_events,
    int num_batches)
{
    /* Zero out counts so re-runs don't see stale data */
    memset(new_counts, 0, sizeof(new_counts));

    ctx->timestamp = (long)time(NULL);
    int event_index = 0;

    for (int batch = 0; batch < num_batches; batch++) {
        // ------------------------------------------------------------------
        // Since we can only monitor 4 events per CHA at a time, we need to
        // program the counters in batches of 4. This loop will iterate over
        // all events and program them in groups of 4.
        int start_idx = batch * NUM_CTR_PER_CHA;
        int num_events_to_program = (start_idx + NUM_CTR_PER_CHA > num_total_events)
                                        ? num_total_events - start_idx
                                        : NUM_CTR_PER_CHA;

        char* event_group[NUM_CTR_PER_CHA];
        for (int i = 0; i < num_events_to_program; i++) {
            event_group[i] = event_name_list[start_idx + i];
        }

        printf("Monitoring session %d/%d: ", batch + 1, num_batches);
        for (int i = 0; i < num_events_to_program; i++) {
            printf("%s ", event_group[i]);
        }
        printf("\n");

        // ------------------------------------------------------------------
        // Set up a PMU monitoring session following documentation:
        //
        // Step (a): Freeze all uncore counters globally.
        // Step (d): Reset counters in each box (done in configure_cha_counters).
        // Step (b) & (c): Program event control registers and enable each monitor.
        // Step (f): Unfreeze counters to begin counting.
        // ------------------------------------------------------------------

        // Step (a): Freeze counters globally before configuration.
        freeze_counters_global(msr_fds, num_sockets);

        // Monitoring session: perform measurements over NUM_RUNS iterations.
        for (int run_idx = 0; run_idx < NUM_RUNS; run_idx++) {
            // Steps (d), (b), (c): Reset counters and program event control
            // registers. This call resets the counters in each CHA (by writing 0x3 to
            // unit control registers) and then programs the control registers with
            // enable (.en), event selection (.ev_sel) and umask bits for each
            // requested event. Note: Currently U_MSR_PMON_UNIT_CTL_rst_both is
            // working. if not, use delta calculation
            configure_cha_counters(msr_fds, num_sockets, events, num_events,
                                   event_group, num_events_to_program);

            // Bench: Preconfigure the benchmark here
            ctx->run_id  = run_idx;
            ctx->iter_id = batch;
            benchmark->init(ctx);

            // Step (f): Unfreeze global counters to start counting.
            unfreeze_counters_global(msr_fds, num_sockets);

            // Bench: Run the benchmark here
            benchmark->roi(ctx);

            // Freeze counters to stop counting at the end of the monitoring interval.
            freeze_counters_global(msr_fds, num_sockets);

            // Run cleanup (if available)
            if (benchmark->cleanup)
                benchmark->cleanup(ctx);

            // Read new counter values after measurement interval.
            read_cha_counters(msr_fds, num_sockets, events, num_events, event_group,
                              num_events_to_program, run_idx, new_counts,
                              event_index);

            // Between iterations: flush all targets and eviction sets out of
            // cache first, then wait 3 seconds to let the system settle.
            if (run_idx < NUM_RUNS - 1) {
                flush_targets_and_evsets();
            }
        }

        event_index += num_events_to_program;
    }

    write_event_counts(new_counts, num_total_events, num_sockets, event_name_list,
                       benchmark->name);
}

/* ── Function to load monitoring counters from a file ────────────────────── */
int load_monitor_counters(char*** event_name_list, int* num_events_to_monitor) {
    FILE* file = fopen("monitor", "r");
    if (!file) {
        perror("Failed to open monitor file");
        return -1;
    }

    char** events = malloc(MAX_MONITOR_EVENTS * sizeof(char*));
    char line[128];
    int count = 0;

    while (fgets(line, sizeof(line), file) && count < MAX_MONITOR_EVENTS) {
        line[strcspn(line, "\n")] = '\0';
        events[count] = strdup(line);
        count++;
    }

    fclose(file);
    *event_name_list = events;
    *num_events_to_monitor = count;
    return 0;
}

/* ── Address annotation + eviction-set validation helpers ───────────────── */
static void print_addr_info(const char *label, void *vaddr)
{
    uint64_t  va  = (uint64_t)(uintptr_t)vaddr;
    uint64_t  pa  = get_pa((uintptr_t)vaddr);
    uintptr_t l1s = L1_SET(pa);
    uintptr_t l2s = L2_SET(pa);
    uintptr_t l3s = L3_SET(pa);

    char bits[19];
    for (int i = 0; i < 18; i++)
        bits[i] = '0' + (int)((pa >> (17 - i)) & 1);
    bits[18] = '\0';

    printf("  %-22s  VA=0x%016lx  PA=0x%016lx\n", label, va, pa);
    printf("    PA[17:0] [%c|%.5s|%.6s|%.6s]"
           "  L1=%-4lu L2=%-6lu L3=%lu\n",
           bits[0], bits + 1, bits + 6, bits + 12, l1s, l2s, l3s);
    printf("            [L3][L2only][ L1  ][offset]\n");
}

/*
 * validate_evset – walk a pointer-chase chain, collect into a flat
 * (non-pointer-chasing) array, then for each entry:
 *   1. Print VA→PA→binary annotation.
 *   2. Verify physical set index matches expected_set.
 *   3. Verify embedded chain link matches next flat entry (or NULL).
 * Prints a pass/fail summary at the end.
 */
static void validate_evset(const char *tag, int level, int expected_set,
                            void *target, void *chain_head, int expected_cnt)
{
    printf("\n=== validate_evset  %s  L%d  set=%d  expected_ways=%d ===\n",
           tag, level, expected_set, expected_cnt);

    /* ── target annotation ── */
    if (target) {
        print_addr_info("target", target);
        uint64_t  pa  = get_pa((uintptr_t)target);
        uintptr_t got = (level == 1) ? L1_SET(pa)
                      : (level == 2) ? L2_SET(pa) : L3_SET(pa);
        printf("    → L%d set: expected=%d  got=%lu  %s\n",
               level, expected_set, got,
               (got == (uintptr_t)expected_set) ? "OK" : "MISMATCH");
    } else {
        printf("  target: NULL (not populated)\n");
    }

    /* ── collect chain into flat (non-pointer-chasing) array ── */
    void *flat[512];
    int   n = 0;
    for (void *p = chain_head; p && n < 512; p = *(void **)p)
        flat[n++] = p;

    printf("  chain length: %d (expected %d)\n", n, expected_cnt);

    /* ── per-entry: annotation + physical set-index check ── */
    int set_err = 0;
    for (int i = 0; i < n; i++) {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "flat[%d]", i);
        print_addr_info(lbl, flat[i]);
        uint64_t  pa  = get_pa((uintptr_t)flat[i]);
        uintptr_t got = (level == 1) ? L1_SET(pa)
                      : (level == 2) ? L2_SET(pa) : L3_SET(pa);
        int ok = (got == (uintptr_t)expected_set);
        printf("    → L%d set: expected=%d  got=%lu  %s\n",
               level, expected_set, got, ok ? "OK" : "MISMATCH");
        if (!ok) set_err++;
    }

    /* ── chain-link verification ── */
    int link_err = 0;
    printf("  chain links:\n");
    for (int i = 0; i < n; i++) {
        void *stored   = *(void **)flat[i];
        void *expected = (i + 1 < n) ? flat[i + 1] : NULL;
        int   ok       = (stored == expected);
        printf("    [%2d] %p  stored→%p  expected→%p  %s\n",
               i, flat[i], stored, expected, ok ? "OK" : "LINK ERR");
        if (!ok) link_err++;
    }

    /* ── summary ── */
    printf("  RESULT: length=%d/%d  set_errs=%d  link_errs=%d  → %s\n",
           n, expected_cnt, set_err, link_err,
           (n == expected_cnt && !set_err && !link_err) ? "PASS" : "FAIL");
    printf("=== end validate_evset ===\n");
    fflush(stdout);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    signal(SIGINT, sigint_handler);
    setvbuf(stdin, NULL, _IONBF, 0);  /* prevent fgets from racing ahead of TUI read() */

    printf("MAX_SOCKETS: %d\n", MAX_SOCKETS);
    load_benchmarks();

    int interactive = 0;
    int no_evset = 0;
    int server = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interactive") == 0) {
            interactive = 1;
        } else if (strcmp(argv[i], "--no-evset") == 0) {
            no_evset = 1;
        } else if (strcmp(argv[i], "--server") == 0) {
            server = 1;
        }
    }

    /* ── One-time setup ── */
    int socket_map[MAX_SOCKETS] = {0};
    int msr_fds[MAX_SOCKETS];
    int num_sockets = find_cpu_sockets(socket_map, MAX_SOCKETS);
    if (num_sockets <= 0) {
        fprintf(stderr, "Error: Could not determine CPU sockets.\n");
        return EXIT_FAILURE;
    }

    if (open_msr_fds(socket_map, num_sockets, msr_fds) != 0) {
        fprintf(stderr, "Error: Failed to open MSR file descriptors.\n");
        return EXIT_FAILURE;
    }

    cha_event_t* events = NULL;
    int num_events = 0;
    if (parse_cha_events(JSON_FILE_PATH, &events, &num_events) != 0) {
        fprintf(stderr, "Error: Failed to parse CHA events.\n");
        return EXIT_FAILURE;
    }

    char** event_name_list = NULL;
    int num_total_events = 0;

    if (!interactive) {
        if (load_monitor_counters(&event_name_list, &num_total_events) != 0) {
            fprintf(stderr, "Error: Failed to load monitoring counters.\n");
            return EXIT_FAILURE;
        }
    } else {
        select_cha_events(events, num_events, &event_name_list, &num_total_events);
    }

    int num_batches = (num_total_events + NUM_CTR_PER_CHA - 1) / NUM_CTR_PER_CHA;

    disable_prefetch(msr_fds, num_sockets);

    find_primary_secondary_cores_per_socket();
    allocate_memory_per_socket();
    set_process_affinity(orchestrator_cores[0]);

    /* Always populate the domain-0 address list (all 28 home CHAs).  benchmark10
     * / Figure 2 read address_list[0][cha]; this is the one-time slow buffer scan
     * the orchestrator keeps alive (via --server) to avoid repeating. */
    generate_cha_mapped_offsets(msr_fds, num_sockets, events, num_events, 0);

    /* Eviction sets are only needed by the evset-based benchmarks; skip them
     * with --no-evset for a faster Figure-2-only startup. */
    if (!no_evset) {
        if (!load_eviction_sets()) {
            build_eviction_sets(msr_fds, num_sockets, events, num_events, 1 /* fast */);
            save_eviction_sets();
        }
        /* Non-CHA-0 L2-demote sets (used by benchmark18). Derived from a buffer
         * scan and not part of the saved evset file, so built after load/build. */
        build_l2_demote_sets(msr_fds, num_sockets, events, num_events, 1 /* fast */);
    }


    BenchmarkCtx ctx = {
        .address_list       = (void*)address_list,
        .primary_cores      = primary_cores,
        .secondary_cores    = secondary_cores,
        .orchestrator_cores = orchestrator_cores,
        .l1_evset           = l1_eviction_sets,
        .l2_evset           = l2_eviction_sets,
        .l3_evset           = l3_eviction_sets,
        .l1_target          = l1_target,
        .l2_target          = l2_target,
        .l3_target          = l3_target,
        .l1_cnt             = l1_cnt,
        .l2_cnt             = l2_cnt,
        .l3_cnt             = l3_cnt,
        .l2_demote_evset    = l2_demote_sets,
        .l2_demote_cnt      = l2_demote_cnt,
        .msr_fds            = msr_fds,
        .arg                = NULL,
    };

    /* ── Server mode ─────────────────────────────────────────────────────────
     * Stay resident after the one-time setup and run benchmarks named on stdin,
     * one per line.  Emits machine-readable sentinels so the Python orchestrator
     * can drive it over a tmux pane:
     *   __SERVER_READY__            setup complete, ready for commands
     *   __BENCH_DONE__ <name>       benchmark finished (outputs written)
     *   __BENCH_ERR__ <name> ...    command could not be run
     * Recognised commands: <benchmark-name>, "list", "quit"/"exit". */
    if (server) {
        setvbuf(stdout, NULL, _IONBF, 0);   /* flush sentinels + progress live */
        printf("__SERVER_READY__\n");
        char line[128];
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *cmd = line;
            while (*cmd == ' ' || *cmd == '\t') cmd++;
            if (*cmd == '\0') continue;

            /* Pick up newly-built or changed benchmark .so files before dispatch,
             * so a freshly compiled benchmark is runnable without a restart. */
            sync_benchmarks();

            if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)
                break;
            if (strcmp(cmd, "list") == 0) {
                list_available_benchmarks();
                printf("__BENCH_DONE__ list\n");
                continue;
            }

            /* Full command is echoed back in the done sentinel so the driver can
             * match a specific invocation (e.g. "benchmark10 15"). */
            char full_cmd[128];
            snprintf(full_cmd, sizeof(full_cmd), "%s", cmd);

            /* Split "<name> <arg>": first whitespace ends the name; the rest
             * (whitespace-trimmed) becomes ctx.arg, or NULL if absent. */
            char *arg = cmd;
            while (*arg && *arg != ' ' && *arg != '\t') arg++;
            if (*arg) { *arg++ = '\0'; while (*arg == ' ' || *arg == '\t') arg++; }
            if (*arg == '\0') arg = NULL;

            int idx = get_benchmark_index(cmd);
            if (idx < 0) {
                printf("__BENCH_ERR__ %s not-found\n", full_cmd);
                continue;
            }

            printf("Running %s ...\n", full_cmd);
            ctx.arg = arg;
            run_benchmark_session(benchmarks[idx], &ctx, msr_fds, num_sockets,
                                  events, num_events,
                                  event_name_list, num_total_events, num_batches);
            ctx.arg = NULL;
            sync_benchmarks();
            printf("__BENCH_DONE__ %s\n", full_cmd);
        }

        unload_all_benchmarks();
        free_cha_events(events, num_events);
        close_msr_fds(msr_fds, num_sockets);
        return EXIT_SUCCESS;
    }

    /* ── Determine starting benchmark ── */
    int current_idx = 0;
    if (argc >= 2 && argv[1][0] != '-') {
        int idx = get_benchmark_index(argv[1]);
        if (idx < 0) {
            printf("Error: Benchmark '%s' not found!\n", argv[1]);
            list_available_benchmarks();
            free_cha_events(events, num_events);
            close_msr_fds(msr_fds, num_sockets);
            unload_all_benchmarks();
            return EXIT_FAILURE;
        }
        current_idx = idx;
    } else {
        /* No benchmark named on the command line: show menu immediately */
        current_idx = select_benchmark_interactive(0);
        if (current_idx < 0) {
            free_cha_events(events, num_events);
            close_msr_fds(msr_fds, num_sockets);
            unload_all_benchmarks();
            return EXIT_SUCCESS;
        }
    }

    /* ── Main run loop ── */
    while (1) {
        Benchmark *benchmark = benchmarks[current_idx];

        run_benchmark_session(benchmark, &ctx, msr_fds, num_sockets,
                              events, num_events,
                              event_name_list, num_total_events,
                              num_batches);

        /* Check for .so changes before showing the menu */
        sync_benchmarks();

        /* Clamp index in case the list shrank */
        if (current_idx >= num_benchmarks)
            current_idx = num_benchmarks - 1;

        current_idx = select_benchmark_interactive(current_idx);
        if (current_idx < 0) break;
    }

    /* ── Cleanup ── */
    unload_all_benchmarks();
    free_cha_events(events, num_events);
    close_msr_fds(msr_fds, num_sockets);

    if (interactive) {
        for (int i = 0; i < num_total_events; i++) {
            free(event_name_list[i]);
        }
        free(event_name_list);
    }

    return EXIT_SUCCESS;
}
