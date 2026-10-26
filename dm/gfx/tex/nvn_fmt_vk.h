// NVNformat → Vulkan format + component remapping, for the Vulkan replay (nvn_vk.c). Pure C, values from vulkan_core.h; same contract as nvn_fmt_mtl.h:
// returns 1 = exact, 2 = substitute (see the table comments), 0 = no counterpart (*vk = 0). swz: the source of output r/g/b/a as NVN_SWZ_* (nvn_fmt_mtl.h), may be NULL.
// Depth formats with 24 bits pick the 24-bit Vulkan format when the device has it (nvn_fmt_vk_depth24), else the 32-bit float one (Apple GPUs under MoltenVK have no D24)
#pragma once
#include <stdint.h>
#include "nvn_fmt_mtl.h"   // NVN_SWZ_*
void nvn_fmt_vk_depth24(int has_x8d24, int has_d24s8);
// BC1–BC7 on a device without textureCompressionBC (Mali, PowerVR): nvn_fmt_to_vk then gives the plain format the texels are decoded into on upload
// (RGBA8 / sRGB, R8, RG8, RGBA16F), and nvn_fmt_vk_decode says which bc_decode.h kind a format decodes as (0 = sampled as it is)
void nvn_fmt_vk_bc(int has_bc);
int nvn_fmt_vk_decode(int nvn_format);
int nvn_fmt_to_vk(int nvn_format, uint32_t *vk_format, uint8_t swz[4]);
// NVN vertex attribute format → Vulkan vertex format (0 = none); *ct gets its component class (0 float / normalized / scaled, 1 uint, 2 sint)
uint32_t nvn_vfmt_to_vk(int nvn_format, int *ct);
