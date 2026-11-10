// spv_nan_minmax (nvn_spv.c) on one module: reads a .spv, writes the patched copy, or prints "none" when the pass has nothing to change.
// With a third argument "select": spv_nan_select instead (the rule written out as OpIsNan + OpSelect).
// Driven by t_nan_minmax.sh, which assembles the test modules (spirv-as), validates what comes out (spirv-val) and checks the disassembly
#include "../nvn/nvn_spv.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) { fprintf(stderr, "usage: t_nan_minmax in.spv out.spv [select]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n); if (!code || fread(code, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    size_t nw = 0; uint32_t *o = argc == 4 ? spv_nan_select(code, (size_t)n / 4, &nw) : spv_nan_minmax(code, (size_t)n / 4, &nw);
    if (!o) { printf("none\n"); return 0; }
    f = fopen(argv[2], "wb"); if (!f || fwrite(o, 4, nw, f) != nw) return 2; fclose(f);
    printf("patched %zu -> %zu words\n", (size_t)n / 4, nw);
    return 0;
}
