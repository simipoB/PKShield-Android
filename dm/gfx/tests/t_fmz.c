// spv_fmz on a SPIR-V binary: t_fmz <in.spv> <out.spv>; exit 0 and the patched module when there was something to patch, 2 and nothing written when there was not
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "nvn_spv.h"
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: t_fmz in.spv out.spv\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(n); if (fread(code, 1, n, f) != (size_t)n) return 1; fclose(f);
    if (getenv("T_GUARD")) {   // the module ending exactly where an unmapped page starts (as Android's allocator places a large block): a read past it faults
        size_t pg = 4096, span = ((size_t)n + pg - 1) / pg * pg; uint8_t *m = mmap(NULL, span + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED || mprotect(m + span, pg, PROT_NONE)) return 1;
        uint32_t *g = (uint32_t *)(m + span - (size_t)n); memcpy(g, code, (size_t)n); free(code); code = g; }
    size_t nw; uint32_t *o = spv_fmz(code, (size_t)n / 4, &nw);
    if (!o) return 2;
    f = fopen(argv[2], "wb"); fwrite(o, 4, nw, f); fclose(f);
    return 0;
}
