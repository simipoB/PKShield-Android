// The BC layer: BC1–BC7 blocks decoded or transcoded (to ETC2 / EAC / B10G11R11 / ASTC) on the GPU, for GPUs without BC textures (Mali, PowerVR), where the CPU
// did it while uploading (bc_decode.c, etc_encode.c). It works on blocks wherever they are, CPU-written or GPU-written: a texture the GPU fills with raw blocks
// (a copy from a buffer or from a texture of 8- or 16-byte texels) converts from the GPU's own data, never from memory the CPU sees stale.
// Self-contained, like a Vulkan layer: it loads its own functions (vkGetDeviceProcAddr), keeps its own pipeline, buffers and descriptor set, and records into the
// caller's command buffer outside any render pass (it binds the compute bind point only, and leaves push constants / descriptors there undefined, as any dispatch does).
// At start it converts known blocks of every kind both ways and compares with the CPU: a kind and mode that does not come out bit for bit stays on the CPU.
#pragma once
#include <stdint.h>
#if __has_include(<vulkan/vulkan.h>)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
typedef struct {
    VkDevice dev; VkQueue queue; uint32_t queue_family;
    PFN_vkGetDeviceProcAddr gdpa;
    const VkPhysicalDeviceMemoryProperties *memp;
    void (*log)(const char *fmt, ...);
} bc_gpu_info;
// builds the layer and checks it on this GPU (one submission on queue: call before the queue is shared); returns 1 when any kind is on. DM_VK_BC_GPU=0: never
int bc_gpu_init(const bc_gpu_info *ii);
// kind (bc_decode.h) in mode 0 (decode) or 1 (transcode) is done here
int bc_gpu_can(int kind, int mode);
// Blocks of one level: src at off, rows of in_pitch bytes (by4 = (h + 3) / 4 rows), nz slices in_slice bytes apart, w x h texels each → into image subresource sub at
// ioff (is3d: the slices are depth, else array layers from sub.baseArrayLayer). The caller has the image in GENERAL. Returns 0 if not done (too big, kind off): the
// caller converts on the CPU
int bc_gpu_buffer_to_image(VkCommandBuffer cb, int kind, int mode, VkBuffer src, VkDeviceSize off, uint32_t in_pitch, uint32_t in_slice, uint32_t w, uint32_t h, uint32_t nz,
                           VkImage img, VkImageSubresourceLayers sub, VkOffset3D ioff, int is3d);
// The same from an image of 8- or 16-byte texels holding blocks (a uint render target the game wrote blocks into): region sub / soff, bw x bh texels = blocks
int bc_gpu_image_to_image(VkCommandBuffer cb, int kind, int mode, VkImage srci, VkImageSubresourceLayers ssub, VkOffset3D soff, uint32_t bw, uint32_t bh, uint32_t nz,
                          uint32_t w, uint32_t h, VkImage img, VkImageSubresourceLayers sub, VkOffset3D ioff, int is3d);
#endif
