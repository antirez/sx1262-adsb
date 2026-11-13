/* Offline experiment. Read one 256-byte RSSI trace on stdin. Try the fast
 * 1090 MHz response at slightly higher amplitude, scoring timing against the
 * whole reply. The transmitted word is deliberately absent from this program.
 * Include the receiver implementation to use the same private trellis and
 * measured tables without adding a research API to the firmware. */
#include "decoder.c"
#include <stdio.h>

enum { KEEP_TIMINGS = 4, SAMPLE_RADIUS = 4 };
typedef struct { sx_trial trial; int cost; } Fit;

static void discard(const Result *r) { (void)r; }

static void keep_fit(Fit best[], int *used, sx_trial trial, int cost) {
    int i = *used;
    if (i == KEEP_TIMINGS && best[i-1].cost <= cost) return;
    if (i == KEEP_TIMINGS) i--;
    else (*used)++;
    while (i && best[i-1].cost > cost) { best[i] = best[i-1]; i--; }
    best[i] = (Fit){trial, cost};
}

/* The 72 stored timing grids include duplicates. At a fixed sample offset
 * two identical grids predict the same waveform, so evaluate it once. */
static bool duplicate_grid(const sx_model *m, int grid) {
    const int16_t *ticks = m->ticks + grid*m->fetches;
    for (int i = 0; i < grid; i++)
        if (same_ticks(ticks, m->ticks + i*m->fetches, m->fetches)) return true;
    return false;
}

/* If two fits give different valid messages, report an ambiguous trace. */
static bool agree(uint8_t result[14], const uint8_t word[14], bool *found) {
    if (*found && memcmp(result, word, 14)) return false;
    memcpy(result, word, 14);
    *found = true;
    return true;
}

static int fit_trace(const uint8_t raw[SAMPLES], int anchor, uint8_t result[14]) {
    Response response = models[FAST1090];
    const sx_model *m = response.table;
    sx_workspace *w = &work->list.base;
    /* Initialize the normal DF17 prefix constraints. Only the five bits
     * specifying DF17 are supplied. The address, data and parity stay free. */
    if (decode_window_mode(raw, SAMPLES, anchor, m, response.size, w, sizeof(*w),
                           NULL, NULL, 112, 1) <= 0) return 0;
    Fit best[KEEP_TIMINGS];
    int used = 0;
    bool found = false;
    uint8_t word[14];
    for (int grid = 0; grid < m->templates; grid++) {
        if (duplicate_grid(m, grid)) continue;
        const int16_t *ticks = m->ticks + grid*m->fetches;
        int first = 0, prefix = 0, body = 0, stop = 0;
        while (ticks[first] < 0) first++;
        while (ticks[prefix] < 13*SX_PHASES) prefix++;
        while (ticks[body] < 8*SX_PHASES) body++;
        while (ticks[stop] < 120*SX_PHASES) stop++;
        for (int at = anchor-SAMPLE_RADIUS; at <= anchor+SAMPLE_RADIUS; at++) {
            if (at < 0 || at+m->fetches > SAMPLES || at+stop+13 > SAMPLES) continue;
            int floor = quiet(raw+at+stop), sum = 0;
            for (int k = body; k < stop; k++) sum += ratio_at(m, floor, raw[at+k]);
            int gain = sum/(stop-body)-256;
            if (gain <= 51) continue;
            gain = (gain*110+50)/100;

            int error = 0;
            for (int k = first; k < prefix; k++) {
                int e = 64*(ratio_at(m, floor, raw[at+k])-256)/gain - m->pre[ticks[k]];
                if (e > 4096) e = 4096;
                if (e < -4096) e = -4096;
                error += e*e;
            }
            error /= prefix-first;
            sx_trial trial = {error > 65535 ? 65535 : error, at, grid, floor, gain};
            if (decode_one(raw, m, w, &trial, word, NULL, 112) &&
                !agree(result, word, &found)) return -1;
            int cost = w->costs[0][0];
            for (int state = 1; state < 64; state++)
                if (w->costs[0][state] < cost) cost = w->costs[0][state];
            keep_fit(best, &used, trial, cost + 2*trial.score);
        }
    }
    for (int j = 0; j < used; j++) {
        work->list.count = 0;
        if ((decode_one(raw, m, w, &best[j].trial, word, &work->list, 112) ||
             ranked_timing(work, word)) && !agree(result, word, &found)) return -1;
    }
    return found;
}

int main(void) {
    uint8_t raw[SAMPLES], word[14];
    if (fread(raw, 1, sizeof(raw), stdin) != sizeof(raw) || fgetc(stdin) != EOF) {
        fputs("Expected one 256-byte RSSI capture on standard input.\n", stderr);
        return 2;
    }
    if (!decoder_init(discard) || !decoder_self_test()) return 2;
    candidate_t peaks[12];
    int n = sx_preambles_model(raw, SAMPLES, shape_data, ASSET_SIZE(shape), peaks, 12, 4, 3500);
    if (!n) return 1;
    int best = 0;
    for (int i = 1; i < n; i++) if (peaks[i].score > peaks[best].score) best = i;
    if (fit_trace(raw, peaks[best].sample, word) != 1) return 1;
    for (size_t i = 0; i < sizeof(word); i++) printf("%02x", word[i]);
    putchar('\n');
    return 0;
}
