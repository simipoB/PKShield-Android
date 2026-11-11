// spv_fmz on a SPIR-V binary: t_fmz <in.spv> <out.spv>; exit 0 and the patched module when there was something to patch, 2 and nothing written when there was not
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "nvn_spv.h"
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: t_fmz in.spv out.spv\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(n); if (fread(code, 1, n, f) != (size_t)n) return 1; fclose(f);
    size_t nw; uint32_t *o = spv_fmz(code, (size_t)n / 4, &nw);
    if (!o) return 2;
    f = fopen(argv[2], "wb"); fwrite(o, 4, nw, f); fclose(f);
    return 0;
}
