// BCn (BC1–BC7) blocks → plain texels on the CPU, for GPUs without BC texture support (Mali, PowerVR, some Xclipse drivers): nvn_vk.c decodes CPU-filled
// textures while uploading them when the device lacks textureCompressionBC. Kinds: 1 BC1, 2 BC2, 3 BC3, 4 BC4, -4 BC4 signed, 5 BC5, -5 BC5 signed,
// 6 BC6H unsigned, -6 BC6H signed, 7 BC7. Output texel: BC1 / BC2 / BC3 / BC7 RGBA8, BC4 R8, BC5 RG8 (signed kinds as two's-complement bytes), BC6H RGBA16F (alpha 1.0)
#pragma once
#include <stddef.h>
#include <stdint.h>
int bc_block_bytes(int kind);   // 8 or 16
int bc_texel_bytes(int kind);   // 1, 2, 4 or 8
// one 4x4 block into out (stride = bytes per output row)
void bc_decode_block(int kind, const uint8_t *blk, uint8_t *out, size_t stride);
// w x h texels: src holds the block rows (src_pitch bytes each), out gets texel rows (out_pitch bytes each); partial edge blocks are cut
void bc_decode_image(int kind, const uint8_t *src, size_t src_pitch, int w, int h, uint8_t *out, size_t out_pitch);
