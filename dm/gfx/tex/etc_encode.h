// BC → ETC2 / EAC on the CPU, for GPUs without BC texture support (Mali, Xclipse, PowerVR): every Android Vulkan GPU samples ETC2 and EAC, which are as small as
// BC, so a texture keeps the size it had on the Switch instead of the 4 to 8 times its plain decode (bc_decode.c) takes. nvn_vk.c uses it when DM_VK_BC_TRANSCODE=etc2.
//   BC1 / BC2 / BC3 → ETC2 RGBA8 (16 bytes a block: EAC alpha, then ETC2 colour; 1 byte a texel)
//   BC4 → EAC R11 (8 bytes a block), BC5 → EAC RG11 (16 bytes: red, then green)
// The other kinds (BC4 / BC5 signed, BC6H, BC7) are not transcoded: bc_transcode_vk returns 0 for them and they keep the plain decode
#pragma once
#include <stddef.h>
#include <stdint.h>
uint32_t bc_transcode_vk(int kind, int srgb);   // the Vulkan format a kind transcodes into, 0 when it does not
int bc_transcode_block_bytes(int kind);          // 8 or 16
// w x h texels of BC blocks (src_pitch bytes a block row) → ETC2 / EAC block rows (out_pitch bytes each)
void bc_transcode_image(int kind, const uint8_t *src, size_t src_pitch, int w, int h, uint8_t *out, size_t out_pitch);
// the encoders, on one 4x4 block of texels (texel (x, y) at [y * 4 + x])
void etc2_encode_rgb(const uint8_t rgba[16][4], uint8_t out[8]);
void eac_encode_a8(const uint8_t v[16], uint8_t out[8]);    // ETC2 RGBA8's alpha block: 8-bit values
void eac_encode_r11(const uint8_t v[16], uint8_t out[8]);   // EAC R11 unsigned, from 8-bit values
