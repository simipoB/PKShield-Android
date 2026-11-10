#include "../../nvn/tex_upload_layout.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "ok" : "FAIL", s); } while (0)

int main(void) {
    // BC6H is 16 bytes per 4x4 block, while decoded RGBA16F is 8 bytes per texel.
    // At 1x1, one raw block is 16 bytes and must not overlap the next cube layer.
    CHECK(nvn_upload_slot(1, 4, 4, 16, 1, 1, 8, 1, 1, 1) == 16,
          "1x1 BC6H slot keeps the larger raw block");
    CHECK(nvn_upload_slot(1, 4, 4, 16, 1, 1, 8, 1, 1, 6) == 96,
          "six 1x1 cube layers have independent raw-block slots");
    CHECK(nvn_upload_slot(0, 4, 4, 16, 1, 1, 8, 1, 1, 6) == 48,
          "non-BC uploads use converted texel size");
    CHECK(nvn_upload_slot(1, 4, 4, 16, 1, 1, 8, 8, 8, 1) == 512,
          "larger levels use the converted size when it is larger");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
