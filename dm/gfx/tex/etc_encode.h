// BC → ETC2 / EAC / ASTC on the CPU, for GPUs without BC texture support (Mali, Xclipse, PowerVR): every Android Vulkan GPU samples ETC2 and EAC, and these
// GPUs ASTC too, which are as small as BC, so a texture keeps the size it had on the Switch instead of the 4 to 8 times its plain decode (bc_decode.c) takes.
// nvn_vk.c uses it when DM_VK_BC_TRANSCODE=etc2.
//   BC1 / BC2 / BC3 → ETC2 RGBA8 (16 bytes a block: EAC alpha, then ETC2 colour; 1 byte a texel)
//   BC4 → EAC R11 (8 bytes a block), BC5 → EAC RG11 (16 bytes: red, then green); the signed kinds → EAC R11 / RG11 SNORM
//   BC7 → ASTC 4x4 (16 bytes a block; only where the GPU has ASTC)
//   BC6H unsigned → B10G11R11 unsigned float texels (4 bytes a texel, half of its RGBA16F decode)
// BC6H signed is not transcoded: bc_transcode_vk returns 0 for it and it keeps the plain decode. Images are split over worker threads (bc_threads)
#pragma once
#include <stddef.h>
#include <stdint.h>
uint32_t bc_transcode_vk(int kind, int srgb, int astc);   // the Vulkan format a kind transcodes into (astc: the GPU has ASTC LDR), 0 when it does not
int bc_transcode_blocks(int kind);   // 1: the output is 4x4 blocks, 0: one texel each (BC6H)
int bc_transcode_bytes(int kind);    // bytes an output block (or texel)
int bc_threads(void);                // threads an image is split over (DM_VK_TRANSCODE_THREADS, else half the cores)
// w x h texels of BC blocks (src_pitch bytes a block row) → output block rows (or texel rows for BC6H), out_pitch bytes each
void bc_transcode_image(int kind, const uint8_t *src, size_t src_pitch, int w, int h, uint8_t *out, size_t out_pitch);
// bc_decode_image, split over the same threads
void bc_decode_image_mt(int kind, const uint8_t *src, size_t src_pitch, int w, int h, uint8_t *out, size_t out_pitch);
// the encoders, on one 4x4 block of texels (texel (x, y) at [y * 4 + x])
void etc2_encode_rgb(const uint8_t rgba[16][4], uint8_t out[8]);
void eac_encode_a8(const uint8_t v[16], uint8_t out[8]);     // ETC2 RGBA8's alpha block: 8-bit values
void eac_encode_r11(const uint8_t v[16], uint8_t out[8]);    // EAC R11 unsigned, from 8-bit values
void eac_encode_r11s(const int8_t v[16], uint8_t out[8]);    // EAC R11 signed, from signed 8-bit values
void astc_encode_rgba(const uint8_t rgba[16][4], uint8_t out[16]);   // ASTC 4x4 LDR
