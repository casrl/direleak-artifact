#include "cc_shared.h"

int node_cpu[4] = {2, 25, 49, 73};

static volatile cc_ctrl *C;
static uint8_t *BASE;
static size_t   BUFB = CC_BUFB;

static volatile uint64_t *tx[CC_MAXW], *bd[CC_MAXW];
static int W, E;

static int wait_phase(int want, long win, int timeout_ms) {
    long deadline = cc_now_ms() + timeout_ms, spins = 0;
    for (;;) {
        if (C->stop) return -1;
        cc_barrier();
        if (C->phase == want && C->win == win) return 0;
        if ((++spins & 0xffff) == 0) { sched_yield(); if (cc_now_ms() > deadline) return -2; }
    }
}

static int wait_flag(volatile int *f, int timeout_ms) {
    long deadline = cc_now_ms() + timeout_ms, spins = 0;
    for (;;) {
        cc_barrier();
        if (*f) return 0;
        if (C->stop) return -1;
        if ((++spins & 0xffff) == 0) { sched_yield(); if (cc_now_ms() > deadline) return -2; }
    }
}

int main(int argc, char **argv) {

    for (int t = 0; t < 200; t++) {
        if (access(CC_SHM_PATH, R_OK | W_OK) == 0) break;
        usleep(50000);
    }
    BASE = cc_map_shared(BUFB, 0);
    if (!BASE) { fprintf(stderr, "[trojan] cannot map shared buffer\n"); return 2; }
    C = (volatile cc_ctrl *)BASE;

    fprintf(stderr, "[trojan] mapped; waiting for spy_ready (spy set-selection is slow)...\n");
    if (wait_flag(&C->spy_ready, 300000) != 0) {
        fprintf(stderr, "[trojan] timed out waiting for spy_ready\n"); return 3;
    }
    if (C->magic != CC_MAGIC) { fprintf(stderr, "[trojan] bad magic\n"); return 4; }

    W = C->W; E = C->E;
    for (int i = 0; i < E; i++) {
        tx[i] = cc_line(BASE, C->troj_tx[i]);
        bd[i] = cc_line(BASE, C->troj_bd[i]);
    }
    int mode = (argc > 1) ? atoi(argv[1]) : C->mode;
    fprintf(stderr, "[trojan] sets read: W=%d E=%d mode=%d nbits=%d "
                    "(tx home CHA%d X=0x%x, bd home CHA%d X=0x%x)\n",
            W, E, mode, C->nbits, C->home_t, C->Xt, C->home_b, C->Xb);

    C->trojan_ready = 1; cc_mfence();
    if (wait_flag(&C->go, 60000) != 0) {
        fprintf(stderr, "[trojan] timed out waiting for go\n"); return 5;
    }
    fprintf(stderr, "[trojan] GO — transmitting %d bits\n", C->nbits);

    int nbits = C->nbits;
    int R = C->R > 0 ? C->R : 1;

    if (mode == 0) {

        for (long w = 0; w < nbits; w++) {
            int bit = C->bits[w];
            int broke = 0;
            for (int r = 0; r < R; r++) {
                long win = w * (long)R + r;
                if (wait_phase(CC_PRIMED, win, 20000) != 0) {
                    fprintf(stderr, "[trojan] lost sync at bit %ld r%d\n", w, r); broke=1; break;
                }
                if (bit) cc_ALLOC_batch(tx, E);
                if (C->use_bd) cc_ALLOC_batch(bd, E);
                cc_mfence();
                C->phase = CC_SENT; cc_mfence();
                if (wait_phase(CC_PROBED, win, 20000) != 0) { broke=1; break; }
                C->phase = CC_READY; cc_mfence();
            }
            if (broke) break;
        }
    } else {

        int hold = C->trojan_hold > 0 ? C->trojan_hold : 200;
        for (long w = 0; w < nbits; w++) {
            if (C->stop) break;
            int bit = C->bits[w];

            if (bit) cc_ALLOC_batch(tx, E);
            cc_ALLOC_batch(bd, E);
            cc_mfence();

            for (int h = 0; h < hold; h++) {
                if (bit) cc_ALLOC_batch(tx, E);
                cc_ALLOC_batch(bd, E);
            }
        }
    }

    fprintf(stderr, "[trojan] done.\n");
    return 0;
}
