/* Offline experiment. Read one 256-byte RSSI trace on stdin. Try the fast
 * 1090 MHz response at slightly higher amplitude, scoring timing against the
 * whole reply. The transmitted word is deliberately absent from this program.
 * Include the receiver implementation to use the same private trellis and
 * measured tables without adding a research API to the firmware. */
#include "decoder.c"
#include <stdio.h>

static void discard(const Result *r) { (void)r; }

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
    if (fit_whole_reply(raw, peaks[best].sample, word) != 1) return 1;
    for (size_t i = 0; i < sizeof(word); i++) printf("%02x", word[i]);
    putchar('\n');
    return 0;
}
