#pragma once

#include <stdint.h>

// A BC upload slot must hold whichever is larger: the raw blocks consumed by
// the GPU decoder, or the converted texels copied to the destination image.
static inline uint64_t nvn_upload_slot(int bc_decode, int bw, int bh, int bpb,
                                       int out_bw, int out_bh, int out_bpb,
                                       uint32_t width, uint32_t height, uint32_t layers) {
    uint64_t out = (uint64_t)((width + out_bw - 1) / out_bw) * (uint64_t)out_bpb *
                   ((height + out_bh - 1) / out_bh);
    uint64_t blocks = (uint64_t)((width + bw - 1) / bw) * (uint64_t)bpb *
                      ((height + bh - 1) / bh);
    return (uint64_t)layers * (bc_decode && blocks > out ? blocks : out);
}
