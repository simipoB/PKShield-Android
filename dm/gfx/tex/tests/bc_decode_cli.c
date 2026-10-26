// bc_decode.c from the command line, for bc_decode_check.py: bc_decode_cli <kind> <width> <height> <blocks in> <texels out>
#include "../bc_decode.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: bc_decode_cli <kind> <width> <height> <blocks in> <texels out>\n"); return 2; }
    int kind = atoi(argv[1]), w = atoi(argv[2]), h = atoi(argv[3]);
    size_t pitch = (size_t)(w + 3) / 4 * bc_block_bytes(kind), n = pitch * ((h + 3) / 4), on = (size_t)w * h * bc_texel_bytes(kind);
    uint8_t *in = malloc(n), *out = malloc(on);
    FILE *f = fopen(argv[4], "rb"); if (!f || fread(in, 1, n, f) != n) { fprintf(stderr, "cannot read %s\n", argv[4]); return 1; } fclose(f);
    bc_decode_image(kind, in, pitch, w, h, out, (size_t)w * bc_texel_bytes(kind));
    f = fopen(argv[5], "wb"); if (!f || fwrite(out, 1, on, f) != on) { fprintf(stderr, "cannot write %s\n", argv[5]); return 1; } fclose(f);
    return 0;
}
