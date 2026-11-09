// The BC layer (bc_gpu.h): bc_gpu/bc_gpu.comp (generated into bc_gpu/bc_gpu_spv.h by bc_gpu/build_spv.sh) run on blocks copied into the layer's own buffer, the
// result copied into the image. Every conversion: a barrier against all earlier work (the layer's buffers are reused, in submission order), the blocks in, the
// dispatch, the texels / blocks out into the image, a barrier before anything reads it
#include "bc_gpu.h"
#if __has_include(<vulkan/vulkan.h>)
#include "bc_decode.h"
#include "etc_encode.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_gpu/bc_gpu_spv.h"

#define FNS(X) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) \
    X(vkCreateFence) X(vkWaitForFences) X(vkDestroyFence) X(vkDestroyBuffer) X(vkFreeMemory)
#define DECL(f) static PFN_##f f;
FNS(DECL)
#undef DECL

enum { IN_CAP = 16 << 20, OUT_CAP = 32 << 20 };   // the largest level converted here: 16 MB of blocks (a 4096x4096 BC7 / BC3 level), 32 MB out; larger ones on the CPU
typedef struct { int32_t kind; uint32_t mode, bw, bh, nz, in_off, in_row, in_slice, out_off, out_row, out_slice, w, h; } pcs;
static VkDevice g_d; static VkPipeline g_pipes[14][2]; static VkShaderModule g_sm; static VkPipelineLayout g_pl; static VkDescriptorSet g_ds; static VkBuffer g_in, g_out;   // (a pipeline per kind + 6 and mode: bc_gpu.comp specialized)
static uint8_t g_ok[16][2];   // [kind + 6][mode]: checked on this GPU
static void (*g_log)(const char *, ...);
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

// DM_VK_BC_GPU_KINDS=6,-6 (a test switch): only the kinds listed go through the layer, the rest on the CPU
static int kind_listed(int kind) {
    static int mask = -1;
    if (mask < 0) { const char *e = getenv("DM_VK_BC_GPU_KINDS"); mask = e && *e ? 0 : 0x3fff;
        for (const char *p = e; p && *p;) { char *q; long k = strtol(p, &q, 10); if (q == p) break; if (k >= -6 && k <= 7) mask |= 1 << (k + 6); p = *q ? q + 1 : q; } }
    return mask >> (kind + 6) & 1;
}
int bc_gpu_can(int kind, int mode) { return kind >= -6 && kind <= 7 && mode >= 0 && mode <= 1 && g_ok[kind + 6][mode] && kind_listed(kind); }
// bytes out per block: decoded, 16 texels; transcoded, the block (EAC R11 8, the rest 16) or BC6H's 16 B10G11R11 texels
static int out_bytes(int kind, int mode) { return !mode ? 16 * bc_texel_bytes(kind) : kind == 6 ? 64 : kind == 4 || kind == -4 ? 8 : 16; }
static int out_blocks(int kind, int mode) { return mode && kind != 6; }

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, const VkPhysicalDeviceMemoryProperties *mp) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) if ((bits >> i & 1) && (mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static int make_buf(VkDeviceSize n, VkBufferUsageFlags use, VkMemoryPropertyFlags want, const VkPhysicalDeviceMemoryProperties *mp, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use };
    if (vkCreateBuffer(g_d, &bi, NULL, b)) return 0;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_d, *b, &rq);
    int t = mem_type(rq.memoryTypeBits, want, mp); if (t < 0 && !map) t = mem_type(rq.memoryTypeBits, 0, mp);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || vkAllocateMemory(g_d, &ai, NULL, m)) { vkDestroyBuffer(g_d, *b, NULL); *b = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(g_d, *b, *m, 0);
    if (map && vkMapMemory(g_d, *m, 0, VK_WHOLE_SIZE, 0, map)) return 0;
    return 1;
}
static void barrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkAccessFlags sa, VkPipelineStageFlags d, VkAccessFlags da) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da };
    vkCmdPipelineBarrier(cb, s, d, 0, 1, &mb, 0, NULL, 0, NULL);
}
// the part every conversion shares once its blocks are in g_in (in_row / in_slice in words): the dispatch into g_out; then out → (img or, for the self-check, a buffer)
static void convert(VkCommandBuffer cb, int kind, int mode, uint32_t bw, uint32_t bh, uint32_t nz, uint32_t in_row, uint32_t in_slice, uint32_t w, uint32_t h) {
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    uint32_t ob = (uint32_t)out_bytes(kind, mode), row = out_blocks(kind, mode) ? bw * ob / 4 : bw * ob / 16;   // (texel rows: 4 per block row)
    pcs p = { kind, (uint32_t)mode, bw, bh, nz, 0, in_row, in_slice, 0, row, row * bh * (out_blocks(kind, mode) ? 1 : 4), w, h };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipes[kind + 6][mode]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_ds, 0, NULL);
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cb, (bw + 7) / 8, (bh + 7) / 8, nz);
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}
static int fits(int kind, int mode, uint32_t bw, uint32_t bh, uint32_t nz, VkDeviceSize in) {
    return bc_gpu_can(kind, mode) && bw && bh && nz && in <= IN_CAP && (VkDeviceSize)out_bytes(kind, mode) * bw * bh * nz <= OUT_CAP;
}
static void out_to_image(VkCommandBuffer cb, uint32_t bw, uint32_t bh, uint32_t nz, uint32_t w, uint32_t h, VkImage img, VkImageSubresourceLayers sub, VkOffset3D ioff, int is3d) {
    sub.layerCount = is3d ? 1 : nz;
    VkBufferImageCopy r = { .bufferOffset = 0, .bufferRowLength = bw * 4, .bufferImageHeight = bh * 4, .imageSubresource = sub, .imageOffset = ioff, .imageExtent = { w, h, is3d ? nz : 1 } };
    vkCmdCopyBufferToImage(cb, g_out, img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
int bc_gpu_buffer_to_image(VkCommandBuffer cb, int kind, int mode, VkBuffer src, VkDeviceSize off, uint32_t in_pitch, uint32_t in_slice, uint32_t w, uint32_t h, uint32_t nz,
                           VkImage img, VkImageSubresourceLayers sub, VkOffset3D ioff, int is3d) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = (uint32_t)bc_block_bytes(kind);
    if (in_pitch % 4 || in_slice % 4 || in_pitch < bw * bb || !fits(kind, mode, bw, bh, nz, (VkDeviceSize)in_slice * nz)) return 0;
    barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy c = { off, 0, (VkDeviceSize)in_slice * (nz - 1) + (VkDeviceSize)in_pitch * bh }; vkCmdCopyBuffer(cb, src, g_in, 1, &c);
    convert(cb, kind, mode, bw, bh, nz, in_pitch / 4, in_slice / 4, w, h);
    out_to_image(cb, bw, bh, nz, w, h, img, sub, ioff, is3d);
    return 1;
}
int bc_gpu_image_to_image(VkCommandBuffer cb, int kind, int mode, VkImage srci, VkImageSubresourceLayers ssub, VkOffset3D soff, uint32_t bw, uint32_t bh, uint32_t nz,
                          uint32_t w, uint32_t h, VkImage img, VkImageSubresourceLayers sub, VkOffset3D ioff, int is3d) {
    uint32_t bb = (uint32_t)bc_block_bytes(kind);
    if (!fits(kind, mode, bw, bh, nz, (VkDeviceSize)bw * bh * bb * nz)) return 0;
    barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    ssub.layerCount = is3d ? 1 : nz;
    VkBufferImageCopy r = { .bufferOffset = 0, .bufferRowLength = bw, .bufferImageHeight = bh, .imageSubresource = ssub, .imageOffset = soff, .imageExtent = { bw, bh, is3d ? nz : 1 } };
    vkCmdCopyImageToBuffer(cb, srci, VK_IMAGE_LAYOUT_GENERAL, g_in, 1, &r);
    convert(cb, kind, mode, bw, bh, nz, bw * bb / 4, bw * bh * bb / 4, w, h);
    out_to_image(cb, bw, bh, nz, w, h, img, sub, ioff, is3d);
    return 1;
}

// ---------- the check on this GPU ----------
static uint32_t rs = 0x9e3779b9u; static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int check(VkQueue q, uint32_t qf, const VkPhysicalDeviceMemoryProperties *mp) {
    enum { BW = 16, BH = 8 }; VkBuffer pb; VkDeviceMemory pm; uint8_t *pp;
    if (!make_buf(1 << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mp, &pb, &pm, (void **)&pp)) return 0;
    VkCommandPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf }; VkCommandPool cp;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f;
    if (vkCreateCommandPool(g_d, &ci, NULL, &cp) || vkCreateFence(g_d, &fi, NULL, &f)) return 0;
    static const int kinds[] = { 1, 2, 3, 4, -4, 5, -5, 6, -6, 7 }; int on = 0; char rep[256]; int rp = 0; rep[0] = 0;
    for (unsigned kk = 0; kk < sizeof kinds / sizeof *kinds; kk++) for (int mode = 0; mode < 2; mode++) {
        int kind = kinds[kk]; if (mode && (kind == -6 || !bc_transcode_vk(kind, 0, 1))) continue;
        uint32_t bb = (uint32_t)bc_block_bytes(kind), ip = BW * bb, ob = (uint32_t)out_bytes(kind, mode), on_ = ob * BW * BH; uint8_t *in = pp, *res = pp + 65536;
        for (uint32_t i = 0; i < ip * BH; i++) in[i] = (uint8_t)rnd();
        for (int b = 0; b < BW * BH / 2; b++) { uint8_t *x = in + b * bb; if (kind == 1) x[1] = x[0] ^ 8; if (kind == 2 || kind == 3) x[9] = x[8] ^ 8; if (kind == 3 || kind == 4 || kind == 5) x[1] = (uint8_t)(x[0] + 3); }   // (smooth blocks too)
        {   // this kind's and mode's pipeline (the shader specialized: KIND, MODE)
            int32_t sc[2] = { kind, mode }; VkSpecializationMapEntry me[2] = { { 0, 0, 4 }, { 1, 4, 4 } }; VkSpecializationInfo si = { 2, me, sizeof sc, sc };
            VkComputePipelineCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = g_sm, .pName = "main", .pSpecializationInfo = &si }, .layout = g_pl };
            VkResult pr = vkCreateComputePipelines(g_d, VK_NULL_HANDLE, 1, &pci, NULL, &g_pipes[kind + 6][mode]);
            if (pr) { g_pipes[kind + 6][mode] = VK_NULL_HANDLE; if (rp < 200) rp += snprintf(rep + rp, sizeof rep - (size_t)rp, " %d/%s (pipeline %d)", kind, mode ? "transcode" : "decode", pr); continue; }
        }
        g_ok[kind + 6][mode] = 1;   // (for fits(): the check runs the real path)
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }; VkCommandBuffer cb;
        if (vkAllocateCommandBuffers(g_d, &ai, &cb)) { g_ok[kind + 6][mode] = 0; continue; }
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; vkBeginCommandBuffer(cb, &bi);
        VkBufferCopy c = { 0, 0, ip * BH }; vkCmdCopyBuffer(cb, pb, g_in, 1, &c);
        convert(cb, kind, mode, BW, BH, 1, ip / 4, ip * BH / 4, BW * 4, BH * 4);
        VkBufferCopy c2 = { 0, 65536, on_ }; vkCmdCopyBuffer(cb, g_out, pb, 1, &c2);
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        vkEndCommandBuffer(cb);
        memset(res, 0xcd, on_);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        int good = !vkQueueSubmit(q, 1, &si, f) && !vkWaitForFences(g_d, 1, &f, VK_TRUE, 5000000000ull);
        vkFreeCommandBuffers(g_d, cp, 1, &cb);
        if (good) {
            uint8_t *cpu = malloc(on_);
            if (!mode) bc_decode_image(kind, in, ip, BW * 4, BH * 4, cpu, (size_t)BW * 4 * bc_texel_bytes(kind));
            else bc_transcode_image(kind, in, ip, BW * 4, BH * 4, cpu, out_blocks(kind, mode) ? (size_t)BW * ob : (size_t)BW * 4 * 4);
            good = !memcmp(cpu, res, on_); free(cpu);
        }
        g_ok[kind + 6][mode] = (uint8_t)good; on |= good;
        if (!good && rp < 200) rp += snprintf(rep + rp, sizeof rep - (size_t)rp, " %d/%s", kind, mode ? "transcode" : "decode");
        vkDestroyFence(g_d, f, NULL); if (vkCreateFence(g_d, &fi, NULL, &f)) break;   // (a fresh fence each time: the layer loads no vkResetFences)
    }
    vkDestroyFence(g_d, f, NULL); vkDestroyBuffer(g_d, pb, NULL); vkFreeMemory(g_d, pm, NULL);
    if (rep[0]) LOG("[RED] BC layer: not the CPU's result on this GPU for kind/mode%s: those stay on the CPU", rep);
    return on;
}
int bc_gpu_init(const bc_gpu_info *ii) {
    const char *e = getenv("DM_VK_BC_GPU"); if (e && !atoi(e)) return 0;
    g_d = ii->dev; g_log = ii->log;
#define LOAD(f) if (!(f = (PFN_##f)ii->gdpa(g_d, #f))) { LOG("BC layer: %s missing", #f); return 0; }
    FNS(LOAD)
#undef LOAD
    VkDeviceMemory mi, mo;
    if (!make_buf(IN_CAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ii->memp, &g_in, &mi, NULL) ||
        !make_buf(OUT_CAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ii->memp, &g_out, &mo, NULL)) { LOG("BC layer: no memory for its buffers"); return 0; }
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof bc_gpu_spv, .pCode = bc_gpu_spv }; VkShaderModule sm;
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb }; VkDescriptorSetLayout dl;
    VkPushConstantRange rg = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcs) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl, .pushConstantRangeCount = 1, .pPushConstantRanges = &rg };
    if (vkCreateShaderModule(g_d, &mci, NULL, &sm) || vkCreateDescriptorSetLayout(g_d, &li, NULL, &dl) || vkCreatePipelineLayout(g_d, &pli, NULL, &g_pl)) { LOG("BC layer: its pipeline layout failed"); return 0; }
    g_sm = sm;   // (the pipelines are made per kind and mode in the check)
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }; VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps }; VkDescriptorPool dp;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &dl };
    if (vkCreateDescriptorPool(g_d, &dpi, NULL, &dp) || (dai.descriptorPool = dp, vkAllocateDescriptorSets(g_d, &dai, &g_ds))) { LOG("BC layer: its descriptor set failed"); return 0; }
    VkDescriptorBufferInfo b0 = { g_in, 0, VK_WHOLE_SIZE }, b1 = { g_out, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b0 },
                                  { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b1 } };
    vkUpdateDescriptorSets(g_d, 2, w, 0, NULL);
    int on = check(ii->queue, ii->queue_family, ii->memp);
    vkDestroyShaderModule(g_d, g_sm, NULL); g_sm = VK_NULL_HANDLE;
    LOG("BC layer: %s (BC textures converted on the GPU, checked against the CPU on this GPU)", on ? "on" : "off: no kind matched the CPU");
    return on;
}
#endif
