/* Give this program saved captures in time order. Labels stay on the caller's
 * side: the decoder only sees the power samples it would have seen in flight. */
#include "receiver.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show(const Result *r) {
    if (!r->length || r->evidence_index) return;
    printf("%"PRIu32" ",r->capture.block);
    for (unsigned i = 0; i < r->length; i++) printf("%02x",r->word[i]);
    printf(" %s\n",r->method);
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}

int main(int argc, char **argv) {
    bool busy = argc==2 && !strcmp(argv[1],"--busy");
    if (argc!=1 && !busy) { fputs("usage: replay [--busy]\n",stderr); return 2; }
    char line[768], hex[513], extra;
    Capture c = {0};
    if (!decoder_init(show) || !decoder_self_test()) return 1;
    while (fgets(line,sizeof(line),stdin)) {
        unsigned floor;
        if (sscanf(line,"%"SCNu32" %"SCNd64" %"SCNd64" %u %512s %c",
                   &c.block,&c.first_ms,&c.last_ms,&floor,hex,&extra)!=5 ||
            strlen(hex)!=2*SAMPLES || floor>255) {
            fputs("Expected: block first_ms last_ms floor 256_hex_bytes\n",stderr);
            return 1;
        }
        c.floor = floor;
        for (int i = 0; i < SAMPLES; i++) {
            int hi = nibble(hex[2*i]), lo = nibble(hex[2*i+1]);
            if (hi < 0 || lo < 0) return 1;
            c.raw[i] = 16*hi+lo;
        }
        if (!busy || decoder_candidate(&c)) decoder_capture(&c);
        while (decoder_idle(c.last_ms+40)) {}
    }
    return ferror(stdin) || ferror(stdout);
}
