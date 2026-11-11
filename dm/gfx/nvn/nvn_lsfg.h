// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
//
// Frame generation with the player's own Lossless Scaling shaders (DM_FRAMEGEN=lsfg). The chain of compute passes below (which pass reads and writes which image, the
// sizes, the samplers and the constants) follows the Eden emulator's Vulkan chain, which is GPL-3.0-or-later; it is rewritten here in C for this engine.
// No shader is part of this file or of the app: the 25 SPIR-V modules come from the player's Lossless.dll, converted once on the device (ls_load reads the cache).
//   ls_load:     reads <dir>/<id>.spv for the 25 shaders and checks each one's bindings against what the chain will give it (refuses anything else)
//   ls_setup:    pipelines once, then the images and descriptor sets for a frame size
//   ls_ingest:   the frame copy c[p] → its 7 flow levels (255), features per level (290..293, three frames of history), the scene's change over time (298..302)
//   ls_generate: motion from the coarsest level to the finest (280, 282..285), detail on the four finest (280, 286..289, 281, 294..297), then the picture m[gen] (256);
//                count pictures between each two frames (1..3: 2x, 3x, 4x), gen 0..count-1 of them, each at its own time between the two
// Like nvn_fgen.h: it owns its images and pipelines, every image stays in the GENERAL layout, a memory barrier comes before each pass, and the caller serializes the
// queue and waits for the device before ls_setup changes size
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef FG_MEM   // the caller's allocator (nvn_vk.c: vk_alloc) or, here, one allocation per image (the same hook as nvn_fgen.h)
#define FG_MEM
typedef struct { VkDeviceMemory mem; } fg_mem;
static int fg_mem_get(VkDevice dev, VkPhysicalDevice pd, VkImage im, fg_mem *m) {
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev, im, &rq); VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((rq.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = i };
        if (vkAllocateMemory(dev, &ai, NULL, &m->mem)) return 0;
        return !vkBindImageMemory(dev, im, m->mem, 0); }
    return 0;
}
static void fg_mem_put(VkDevice dev, fg_mem m) { if (m.mem) vkFreeMemory(dev, m.mem, NULL); }
#endif
#define LS_MIPS 7
#define LS_NSH 25    // 255, 256, 280..302
#define LS_NSET 148
#define LS_NIMG 160
#define LS_MAXGEN 3   // pictures between two frames at most (4x)
#define LS_BLOCKS 7   // constants: block 0 time 0; blocks 1..6 Eden's generation slots, (count - 1) * count / 2 + gen, time (gen + 1) / (count + 1); 3 kinds each
#define LS_IX(id) ((id) < 280 ? (id) - 255 : (id) - 278)
static int ls_id(int ix) { return ix < 2 ? 255 + ix : 278 + ix; }
// What each shader binds, in binding order (set 0): uniform buffers, samplers, sampled images, storage images
static const uint8_t LS_CNT[LS_NSH][4] = {
    { 1, 1, 1, 7 }, { 1, 2, 5, 1 },                                                                                                    // 255 levels, 256 the picture
    { 1, 2, 5, 3 }, { 1, 2, 6, 1 }, { 0, 1, 3, 2 }, { 0, 1, 2, 2 }, { 0, 1, 2, 2 }, { 1, 2, 4, 1 }, { 0, 1, 3, 2 }, { 0, 1, 2, 2 }, { 0, 1, 2, 2 }, { 1, 2, 4, 1 },   // 280..289
    { 0, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1, 2 }, { 0, 1, 2, 2 }, { 0, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1, 1 }, { 1, 2, 2, 1 },   // 290..297
    { 0, 1, 6, 2 }, { 0, 1, 2, 2 }, { 0, 1, 2, 2 }, { 0, 1, 2, 2 }, { 1, 1, 2, 6 } };                                                 // 298..302
enum { LS_S_ZERO, LS_S_WHITE, LS_S_EDGE };   // samplers: outside the image is transparent black / opaque white / the edge texel
typedef struct { VkImage img; VkImageView view, pview; fg_mem mem; int has; uint32_t w, h; } ls_img;   // (pview: the picture in the frame's own format, for the caller)
typedef struct {
    VkDevice dev; VkPhysicalDevice pd; VkFormat fmt, ufmt; uint32_t w, h, fw, fh; float flow; int base, failed, fresh, p, hist, stripped; uint64_t n; const char *why;
    uint32_t *spv[LS_NSH]; size_t nspv[LS_NSH];   // the player's shaders (bytes)
    VkSampler samp[3]; VkDescriptorSetLayout dsl[LS_NSH]; VkPipelineLayout pl[LS_NSH]; VkPipeline pipe[LS_NSH]; VkDescriptorPool dp;
    VkBuffer ubo; VkDeviceMemory umem; uint8_t *umap; uint32_t ustride;   // LS_BLOCKS blocks of 3 kinds: plain, "first motion pass", "first detail pass" (bound as a dynamic offset)
    ls_img *all[LS_NIMG]; int nall;
    ls_img c[2], m[LS_MAXGEN], zero, mip[LS_MIPS];   // frame copies (swapping), the in-between pictures, a black texel for "no coarser level", the flow levels
    struct { ls_img t1, t2, t3[2], out[3][2]; VkDescriptorSet ds[3], last[3]; } al[LS_MIPS];
    struct { ls_img t1[2], t2[2], out[6]; VkDescriptorSet first[3], ds[4]; } be;
    struct { ls_img t1[3], t2[2], out; VkDescriptorSet first[3], ds[4]; } ga[LS_MIPS];
    struct { ls_img t1[3], t2[2], out1, out2; VkDescriptorSet first[3], ds[4], sixth[3], ds2[4]; } de[3];
    VkDescriptorSet ds_mip[2], ds_gen[2][LS_MAXGEN];
} lsfg;
static int ls_fail(lsfg *g, const char *why) { g->failed = 1; g->why = why; return 0; }

// ---- the shaders: loaded from the cache, each checked before use ----
// NULL when the module is a compute shader "main" whose set 0 holds exactly the bindings LS_CNT gives shader ix, in that order
static const char *ls_spv_check(const uint32_t *w, size_t n, int ix) {
    if (n < 5 || w[0] != 0x07230203u) return "not a SPIR-V module";
    uint32_t bound = w[3]; if (!bound || bound > 1u << 20) return "a bad id bound";
    typedef struct { uint8_t kind; uint32_t pointee, vtype; int32_t set, bind; } idinfo;
    idinfo *d = calloc(bound, sizeof *d); if (!d) return "no memory";
    for (uint32_t i = 0; i < bound; i++) d[i].set = d[i].bind = -1;
    const char *bad = NULL; int entry = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16; if (!len || i + len > n) break;   // (every id an instruction names first, against the bound)
        for (uint32_t k = 1; k < len && k < 4; k++) { uint32_t op = w[i] & 0xffff; if ((op == 71 || op == 30 || op == 26 || op == 25 || op == 32 || op == 59) && k < 3 && w[i + k] >= bound && !(op == 71 && k == 2)) bad = "an id out of range"; }
        i += len; }
#define ID(x) ((x) < bound ? (x) : 0)
    for (size_t i = 5; i < n && !bad; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len || i + len > n) { bad = "a broken instruction"; break; }
        const uint32_t *a = w + i;
        if (op == 15 && len >= 5) { if (a[1] != 5 || a[3] != 0x6e69616du || (a[4] & 0xff)) bad = "not a compute shader called main"; entry++; }
        else if (op == 71 && len >= 4) { if (a[2] == 33) d[ID(a[1])].bind = (int32_t)a[3]; else if (a[2] == 34) d[ID(a[1])].set = (int32_t)a[3]; }
        else if (op == 30 && len >= 2) d[ID(a[1])].kind = 1;
        else if (op == 26 && len >= 2) d[ID(a[1])].kind = 2;
        else if (op == 25 && len >= 9) d[ID(a[1])].kind = a[7] == 1 ? 3 : a[7] == 2 ? 4 : 0;
        else if (op == 32 && len >= 4) d[ID(a[1])].pointee = ID(a[3]);
        else if (op == 59 && len >= 4) d[ID(a[2])].vtype = ID(a[1]);
        i += len; }
#undef ID
    if (!bad && entry != 1) bad = "not one entry point";
    const uint8_t *c = LS_CNT[ix]; int total = c[0] + c[1] + c[2] + c[3]; uint8_t got[16] = { 0 };
    for (uint32_t i = 0; i < bound && !bad; i++) if (d[i].vtype && (d[i].set >= 0 || d[i].bind >= 0)) {
        if (d[i].set != 0 || d[i].bind < 0 || d[i].bind >= total || got[d[i].bind]) { bad = "bindings the chain does not give"; break; }
        got[d[i].bind] = d[d[d[i].vtype].pointee].kind ? d[d[d[i].vtype].pointee].kind : 9; }
    for (int b = 0, k = 0; k < 4 && !bad; k++) for (int j = 0; j < c[k]; j++, b++) if (got[b] != k + 1) { bad = "a binding of another kind than the chain gives"; break; }
    free(d); return bad;
}
static void ls_unload(lsfg *g) { for (int i = 0; i < LS_NSH; i++) { free(g->spv[i]); g->spv[i] = NULL; g->nspv[i] = 0; } }
// The 25 converted shaders from a folder (<id>.spv): 1, or 0 with g->why (nothing kept)
static int ls_load(lsfg *g, const char *dir) {
    static char why[160];
    for (int ix = 0; ix < LS_NSH; ix++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%d.spv", dir, ls_id(ix)); FILE *f = fopen(p, "rb");
        if (!f) { snprintf(why, sizeof why, "shader %d is missing", ls_id(ix)); g->why = why; ls_unload(g); return 0; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint32_t *w = sz >= 20 && sz < (16 << 20) && !(sz & 3) ? malloc((size_t)sz) : NULL; size_t rd = w ? fread(w, 1, (size_t)sz, f) : 0; fclose(f);
        const char *bad = !w || rd != (size_t)sz ? "cannot be read" : ls_spv_check(w, (size_t)sz / 4, ix);
        if (bad) { free(w); snprintf(why, sizeof why, "shader %d: %s", ls_id(ix), bad); g->why = why; ls_unload(g); return 0; }
        free(g->spv[ix]); g->spv[ix] = w; g->nspv[ix] = (size_t)sz; }
    return 1;
}
// The float controls the converted shaders declare (how the smallest numbers and halfway cases round), taken out for a driver without them: without this it would
// refuse the shaders, and the picture differs only in the last bit of a few values (the PC test, lavapipe). In place; returns the new word count
static size_t ls_spv_strip_float_controls(uint32_t *w, size_t n) {
    size_t o = 5;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len || i + len > n) return n;
        int drop = (op == 17 && len >= 2 && w[i + 1] >= 4464 && w[i + 1] <= 4468)      // OpCapability DenormPreserve .. RoundingModeRTZ
                || (op == 16 && len >= 3 && w[i + 2] >= 4459 && w[i + 2] <= 4463);     // OpExecutionMode DenormPreserve .. RoundingModeRTZ
        if (!drop) { memmove(w + o, w + i, len * 4); o += len; }
        i += len; }
    return o;
}
// What the converted shaders need of the driver (the device must have been created with these features on): NULL when it has all of it.
// precise 0: the float controls (rounding, denormals) are not asked; a driver without them only rounds the smallest numbers differently (the PC test on lavapipe)
static const char *ls_device_lacks(VkPhysicalDevice pd, int precise) {
    VkPhysicalDeviceVulkan12Properties p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12 };
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_3) return "Vulkan 1.3";
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    vkGetPhysicalDeviceFeatures2(pd, &f2); vkGetPhysicalDeviceProperties2(pd, &p2);
    if (!f12.bufferDeviceAddress) return "buffer device addresses";
    if (!f12.vulkanMemoryModel) return "the Vulkan memory model";
    if (!f12.shaderFloat16) return "16-bit floats in shaders";
    if (!f13.maintenance4) return "maintenance4";
    if (!f2.features.shaderStorageImageWriteWithoutFormat) return "storage image writes without a format";
    if (!precise) return NULL;
    if (!p12.shaderRoundingModeRTEFloat16 || !p12.shaderRoundingModeRTEFloat32) return "round-to-even float control";
    if (!p12.shaderDenormFlushToZeroFloat32 || !p12.shaderDenormPreserveFloat16) return "denormal float control";
    return NULL;
}

// ---- images ----
static void ls_images_free(lsfg *g) {
    for (int k = 0; k < g->nall; k++) { ls_img *i = g->all[k];
        if (i->pview && i->pview != i->view) vkDestroyImageView(g->dev, i->pview, NULL);
        if (i->view) vkDestroyImageView(g->dev, i->view, NULL);
        if (i->img) vkDestroyImage(g->dev, i->img, NULL);
        if (i->has) fg_mem_put(g->dev, i->mem);
        memset(i, 0, sizeof *i); }
    g->nall = 0; g->w = g->h = 0;
}
// pfmt: the format the caller will sample it in (the frame's own, when that is the sRGB twin of fmt)
static int ls_img_make(lsfg *g, ls_img *i, VkFormat fmt, uint32_t w, uint32_t h, VkFormat pfmt) {
    if (g->nall >= LS_NIMG) return 0;
    w = w ? w : 1; h = h ? h : 1; g->all[g->nall++] = i;
    // (pfmt not fmt: the image is the frame's sRGB format, so a blit or copy of it converts right; the shaders write it through a UNORM view)
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = pfmt != fmt ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = pfmt,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    if (vkCreateImage(g->dev, &ci, NULL, &i->img)) { i->img = VK_NULL_HANDLE; return 0; }
    if (!fg_mem_get(g->dev, g->pd, i->img, &i->mem)) return 0;
    i->has = 1;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(g->dev, &vci, NULL, &i->view)) { i->view = VK_NULL_HANDLE; return 0; }
    i->pview = i->view;
    if (pfmt != fmt) { VkImageViewUsageCreateInfo uu = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_SAMPLED_BIT }; vci.pNext = &uu; vci.format = pfmt;
        if (vkCreateImageView(g->dev, &vci, NULL, &i->pview)) { i->pview = VK_NULL_HANDLE; return 0; } }
    i->w = w; i->h = h; return 1;
}
static VkFormat ls_unorm(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f == VK_FORMAT_A8B8G8R8_SRGB_PACK32 ? VK_FORMAT_A8B8G8R8_UNORM_PACK32 : f;
}
static int ls_fmt_ok(lsfg *g, VkFormat f) {
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g->pd, f, &fp); return (fp.optimalTilingFeatures & need) == need;
}

// ---- descriptor sets ----
static VkDescriptorSet ls_set(lsfg *g, int id) {
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g->dp, .descriptorSetCount = 1, .pSetLayouts = &g->dsl[LS_IX(id)] };
    VkDescriptorSet s = VK_NULL_HANDLE; if (vkAllocateDescriptorSets(g->dev, &ai, &s)) { g->failed = 1; return VK_NULL_HANDLE; }
    return s;
}
// One set of shader `id`: its constants (which kind: 0 plain, 1 first motion pass, 2 first detail pass; the block, the time, comes at binding), its first sampler (the second is always the edge one), the images it reads and the images it writes
static void ls_write(lsfg *g, VkDescriptorSet ds, int id, int ubo, int s0, ls_img **tex, ls_img **sto) {
    const uint8_t *c = LS_CNT[LS_IX(id)]; VkWriteDescriptorSet w[16]; VkDescriptorImageInfo ii[16]; uint32_t n = 0;
    VkDescriptorBufferInfo bi = { g->ubo, (VkDeviceSize)ubo * g->ustride, 48 };
#define LS_W(type) w[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = n, .descriptorCount = 1, .descriptorType = type, .pImageInfo = &ii[n], .pBufferInfo = &bi }; n++
    if (c[0]) { ii[n] = (VkDescriptorImageInfo){ 0 }; LS_W(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC); }
    for (int k = 0; k < c[1]; k++) { ii[n] = (VkDescriptorImageInfo){ g->samp[k ? LS_S_EDGE : s0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED }; LS_W(VK_DESCRIPTOR_TYPE_SAMPLER); }
    for (int k = 0; k < c[2]; k++) { ii[n] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, tex[k]->view, VK_IMAGE_LAYOUT_GENERAL }; LS_W(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); }
    for (int k = 0; k < c[3]; k++) { ii[n] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, sto[k]->view, VK_IMAGE_LAYOUT_GENERAL }; LS_W(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); }
#undef LS_W
    vkUpdateDescriptorSets(g->dev, n, w, 0, NULL);
}
#define LSV(...) ((ls_img *[]){ __VA_ARGS__ })

// Samplers, one pipeline per shader, every descriptor set, the constants' buffer: once
static int ls_base(lsfg *g) {
    VkDevice dev = g->dev;
    for (int ix = 0; ix < LS_NSH; ix++) if (!g->spv[ix]) { g->why = "the Lossless Scaling shaders are not imported"; return 0; }
    for (int s = 0; s < 3; s++) { VkSamplerAddressMode am = s == LS_S_EDGE ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = am, .addressModeV = am, .addressModeW = am, .maxLod = VK_LOD_CLAMP_NONE, .borderColor = s == LS_S_WHITE ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
        if (vkCreateSampler(dev, &sci, NULL, &g->samp[s])) return ls_fail(g, "no sampler"); }
    for (int ix = 0; ix < LS_NSH; ix++) {
        const uint8_t *c = LS_CNT[ix]; VkDescriptorSetLayoutBinding b[16]; uint32_t n = 0;
        static const VkDescriptorType T[4] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
        for (int k = 0; k < 4; k++) for (int j = 0; j < c[k]; j++, n++) b[n] = (VkDescriptorSetLayoutBinding){ n, T[k], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b };
        if (vkCreateDescriptorSetLayout(dev, &dli, NULL, &g->dsl[ix])) return ls_fail(g, "no descriptor layout");
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g->dsl[ix] };
        if (vkCreatePipelineLayout(dev, &pli, NULL, &g->pl[ix])) return ls_fail(g, "no pipeline layout");
        if (g->stripped) g->nspv[ix] = ls_spv_strip_float_controls(g->spv[ix], g->nspv[ix] / 4) * 4;
        VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g->nspv[ix], .pCode = g->spv[ix] }; VkShaderModule mod;
        if (vkCreateShaderModule(dev, &mi, NULL, &mod)) return ls_fail(g, "the driver refused a shader");
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = g->pl[ix],
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" } };
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &g->pipe[ix]); vkDestroyShaderModule(dev, mod, NULL);
        if (r) { g->pipe[ix] = VK_NULL_HANDLE; return ls_fail(g, "the driver could not build a shader"); } }
    VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, LS_NSET }, { VK_DESCRIPTOR_TYPE_SAMPLER, LS_NSET * 2 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, LS_NSET * 6 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, LS_NSET * 7 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = LS_NSET, .poolSizeCount = 4, .pPoolSizes = ps };
    if (vkCreateDescriptorPool(dev, &dpi, NULL, &g->dp)) return ls_fail(g, "no descriptor pool");
    for (int i = 0; i < 2; i++) { g->ds_mip[i] = ls_set(g, 255); for (int k = 0; k < LS_MAXGEN; k++) g->ds_gen[i][k] = ls_set(g, 256); }
    for (int l = 0; l < LS_MIPS; l++) { for (int k = 0; k < 3; k++) { g->al[l].ds[k] = ls_set(g, 290 + k); g->al[l].last[k] = ls_set(g, 293); } }
    for (int k = 0; k < 3; k++) g->be.first[k] = ls_set(g, 298);
    for (int k = 0; k < 4; k++) g->be.ds[k] = ls_set(g, 299 + k);
    for (int i = 0; i < LS_MIPS; i++) { for (int k = 0; k < 3; k++) g->ga[i].first[k] = ls_set(g, 280); for (int k = 0; k < 4; k++) g->ga[i].ds[k] = ls_set(g, 282 + k); }
    for (int i = 0; i < 3; i++) { for (int k = 0; k < 3; k++) { g->de[i].first[k] = ls_set(g, 280); g->de[i].sixth[k] = ls_set(g, 281); }
                                  for (int k = 0; k < 4; k++) { g->de[i].ds[k] = ls_set(g, 286 + k); g->de[i].ds2[k] = ls_set(g, 294 + k); } }
    if (g->failed) return ls_fail(g, "no descriptor sets");
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->pd, &pp); VkDeviceSize al = pp.limits.minUniformBufferOffsetAlignment; g->ustride = al > 256 ? (uint32_t)al : 256;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)LS_BLOCKS * 3 * g->ustride, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
    if (vkCreateBuffer(dev, &bci, NULL, &g->ubo)) return ls_fail(g, "no buffer for its constants");
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, g->ubo, &rq); VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g->pd, &mp); uint32_t t = 0;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (t < mp.memoryTypeCount && !((rq.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & hv) == hv)) t++;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t }; void *m = NULL;
    if (t == mp.memoryTypeCount || vkAllocateMemory(dev, &mai, NULL, &g->umem) || vkBindBufferMemory(dev, g->ubo, g->umem, 0) || vkMapMemory(dev, g->umem, 0, VK_WHOLE_SIZE, 0, &m))
        return ls_fail(g, "no memory for its constants");
    g->umap = m; g->base = 1; return 1;
}
// Ready for frames of this format and size, the motion search at `flow` of the frame's size (0.25..1): 1, or 0 with g->why (g->failed: it cannot run at all, stop asking).
// A new size, format or flow scale destroys the images: the device must be idle
static int ls_setup(lsfg *g, VkDevice dev, VkPhysicalDevice pd, VkFormat fmt, uint32_t w, uint32_t h, float flow) {
    if (g->failed) return 0;
    flow = flow < 0.25f ? 0.25f : flow > 1 ? 1 : flow;
    if (g->w == w && g->h == h && g->fmt == fmt && g->flow == flow) return 1;
    g->dev = dev; g->pd = pd;
    if (!g->base) {
        const char *lack = ls_device_lacks(pd, 0); if (lack) { static char why[96]; snprintf(why, sizeof why, "the graphics driver lacks %s", lack); return ls_fail(g, why); }
        g->stripped = ls_device_lacks(pd, 1) != NULL;   // (no float controls: the shaders go without theirs, ls_spv_strip_float_controls)
        if (!ls_base(g)) return 0; }
    if (w < 64 || h < 64) { g->why = "the frame is too small"; return 0; }
    const VkFormat u = ls_unorm(fmt), R8 = VK_FORMAT_R8_UNORM, C8 = VK_FORMAT_R8G8B8A8_UNORM, F16 = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!ls_fmt_ok(g, u) || !ls_fmt_ok(g, R8) || !ls_fmt_ok(g, C8) || !ls_fmt_ok(g, F16)) return ls_fail(g, "the driver cannot use the frame's format, R8, RGBA8 or RGBA16F as a compute target");
    ls_images_free(g);
    uint32_t fw = (uint32_t)((float)w * flow), fh = (uint32_t)((float)h * flow); fw = fw ? fw : 1; fh = fh ? fh : 1;
    int ok = 1;
#define MK(i, f, ww, hh) ok = ok && ls_img_make(g, i, f, ww, hh, f)
    MK(&g->zero, F16, 1, 1); ok = ok && ls_img_make(g, &g->m[0], u, w, h, fmt) && ls_img_make(g, &g->m[1], u, w, h, fmt) && ls_img_make(g, &g->m[2], u, w, h, fmt) && ls_img_make(g, &g->c[0], u, w, h, fmt) && ls_img_make(g, &g->c[1], u, w, h, fmt);
    for (int l = 0; l < LS_MIPS; l++) MK(&g->mip[l], R8, fw >> l, fh >> l);
    for (int l = 0; l < LS_MIPS && ok; l++) { uint32_t hw = (g->mip[l].w + 1) >> 1, hh = (g->mip[l].h + 1) >> 1, qw = (hw + 1) >> 1, qh = (hh + 1) >> 1;
        MK(&g->al[l].t1, C8, hw, hh); MK(&g->al[l].t2, C8, hw, hh); MK(&g->al[l].t3[0], C8, qw, qh); MK(&g->al[l].t3[1], C8, qw, qh);
        for (int k = 0; k < 6; k++) MK(&g->al[l].out[k / 2][k & 1], C8, qw, qh); }
    if (ok) { uint32_t ew = g->al[0].out[0][0].w, eh = g->al[0].out[0][0].h;
        for (int k = 0; k < 2; k++) { MK(&g->be.t1[k], C8, ew, eh); MK(&g->be.t2[k], C8, ew, eh); }
        for (int k = 0; k < 6; k++) MK(&g->be.out[k], R8, ew >> k, eh >> k); }
    for (int i = 0; i < LS_MIPS && ok; i++) { ls_img *e = &g->al[LS_MIPS - 1 - i].out[0][0];
        for (int k = 0; k < 3; k++) MK(&g->ga[i].t1[k], C8, e->w, e->h);
        for (int k = 0; k < 2; k++) MK(&g->ga[i].t2[k], C8, e->w, e->h);
        MK(&g->ga[i].out, F16, e->w, e->h); }
    for (int i = 0; i < 3 && ok; i++) { ls_img *e = &g->al[2 - i].out[0][0];
        for (int k = 0; k < 3; k++) MK(&g->de[i].t1[k], C8, e->w, e->h);
        for (int k = 0; k < 2; k++) MK(&g->de[i].t2[k], C8, e->w, e->h);
        MK(&g->de[i].out1, F16, e->w, e->h); MK(&g->de[i].out2, F16, e->w, e->h); }
#undef MK
    if (!ok) { ls_images_free(g); return ls_fail(g, "no memory for its images"); }
    for (int b = 0; b < LS_BLOCKS; b++) {   // block b > 0: generation slot b - 1 (count 1: slot 0; count 2: slots 1, 2; count 3: slots 3..5)
        int slot = b - 1, cnt = slot < 1 ? 1 : slot < 3 ? 2 : 3, gen = slot - (cnt - 1) * cnt / 2; float ts = b ? (float)(gen + 1) / (float)(cnt + 1) : 0;
        for (int v = 0; v < 3; v++) { struct { uint32_t off[2], first, first_s, color, hdr; float inv, ts, ui; uint32_t pad[3]; } k = { .first = v == 1, .first_s = v == 2, .inv = 1.0f / flow, .ts = ts, .ui = 0.5f };
            memcpy(g->umap + (size_t)(b * 3 + v) * g->ustride, &k, 48); } }
    for (int i = 0; i < 2; i++) {
        ls_write(g, g->ds_mip[i], 255, 0, LS_S_ZERO, LSV(&g->c[i]), LSV(&g->mip[0], &g->mip[1], &g->mip[2], &g->mip[3], &g->mip[4], &g->mip[5], &g->mip[6]));
        for (int k = 0; k < LS_MAXGEN; k++) ls_write(g, g->ds_gen[i][k], 256, 0, LS_S_ZERO, LSV(&g->c[1 - i], &g->c[i], &g->ga[6].out, &g->de[2].out1, &g->de[2].out2), LSV(&g->m[k])); }
    for (int l = 0; l < LS_MIPS; l++) {
        ls_write(g, g->al[l].ds[0], 290, 0, LS_S_ZERO, LSV(&g->mip[l]), LSV(&g->al[l].t1));
        ls_write(g, g->al[l].ds[1], 291, 0, LS_S_ZERO, LSV(&g->al[l].t1), LSV(&g->al[l].t2));
        ls_write(g, g->al[l].ds[2], 292, 0, LS_S_ZERO, LSV(&g->al[l].t2), LSV(&g->al[l].t3[0], &g->al[l].t3[1]));
        for (int k = 0; k < 3; k++) ls_write(g, g->al[l].last[k], 293, 0, LS_S_ZERO, LSV(&g->al[l].t3[0], &g->al[l].t3[1]), LSV(&g->al[l].out[k][0], &g->al[l].out[k][1])); }
    { ls_img (*in)[2] = g->al[0].out;
      for (int k = 0; k < 3; k++) { int a = (k + 1) % 3, b = (k + 2) % 3;
          ls_write(g, g->be.first[k], 298, 0, LS_S_WHITE, LSV(&in[a][0], &in[a][1], &in[b][0], &in[b][1], &in[k][0], &in[k][1]), LSV(&g->be.t1[0], &g->be.t1[1])); }
      ls_write(g, g->be.ds[0], 299, 0, LS_S_ZERO, LSV(&g->be.t1[0], &g->be.t1[1]), LSV(&g->be.t2[0], &g->be.t2[1]));
      ls_write(g, g->be.ds[1], 300, 0, LS_S_ZERO, LSV(&g->be.t2[0], &g->be.t2[1]), LSV(&g->be.t1[0], &g->be.t1[1]));
      ls_write(g, g->be.ds[2], 301, 0, LS_S_ZERO, LSV(&g->be.t1[0], &g->be.t1[1]), LSV(&g->be.t2[0], &g->be.t2[1]));
      ls_write(g, g->be.ds[3], 302, 0, LS_S_ZERO, LSV(&g->be.t2[0], &g->be.t2[1]), LSV(&g->be.out[0], &g->be.out[1], &g->be.out[2], &g->be.out[3], &g->be.out[4], &g->be.out[5])); }
    for (int i = 0; i < LS_MIPS; i++) { int L = LS_MIPS - 1 - i, ub = i ? 0 : 1; ls_img (*in)[2] = g->al[L].out, *prev = i ? &g->ga[i - 1].out : &g->zero, *flw = &g->be.out[L < 5 ? L : 5], *t1 = g->ga[i].t1, *t2 = g->ga[i].t2;
        for (int k = 0; k < 3; k++) { int a = (k + 2) % 3; ls_write(g, g->ga[i].first[k], 280, ub, LS_S_WHITE, LSV(&in[a][0], &in[a][1], &in[k][0], &in[k][1], prev), LSV(&t1[0], &t1[1], &t1[2])); }
        ls_write(g, g->ga[i].ds[0], 282, 0, LS_S_ZERO, LSV(&t1[0], &t1[1], &t1[2]), LSV(&t2[0], &t2[1]));
        ls_write(g, g->ga[i].ds[1], 283, 0, LS_S_ZERO, LSV(&t2[0], &t2[1]), LSV(&t1[0], &t1[1]));
        ls_write(g, g->ga[i].ds[2], 284, 0, LS_S_ZERO, LSV(&t1[0], &t1[1]), LSV(&t2[0], &t2[1]));
        ls_write(g, g->ga[i].ds[3], 285, ub, LS_S_ZERO, LSV(&t2[0], &t2[1], prev, flw), LSV(&g->ga[i].out)); }
    for (int i = 0; i < 3; i++) { int L = 2 - i, ub = i ? 0 : 2; ls_img (*in)[2] = g->al[L].out, *pg = i ? &g->ga[i + 3].out : &g->zero, *p1 = i ? &g->de[i - 1].out1 : &g->zero, *p2 = i ? &g->de[i - 1].out2 : &g->zero;
        ls_img *flw = &g->be.out[L], *t1 = g->de[i].t1, *t2 = g->de[i].t2;
        for (int k = 0; k < 3; k++) { int a = (k + 2) % 3;
            ls_write(g, g->de[i].first[k], 280, ub, LS_S_WHITE, LSV(&in[a][0], &in[a][1], &in[k][0], &in[k][1], pg), LSV(&t1[0], &t1[1], &t1[2]));
            ls_write(g, g->de[i].sixth[k], 281, ub, LS_S_WHITE, LSV(&in[a][0], &in[a][1], &in[k][0], &in[k][1], pg, p1), LSV(&t2[0])); }
        ls_write(g, g->de[i].ds[0], 286, 0, LS_S_ZERO, LSV(&t1[0], &t1[1], &t1[2]), LSV(&t2[0], &t2[1]));
        ls_write(g, g->de[i].ds[1], 287, 0, LS_S_ZERO, LSV(&t2[0], &t2[1]), LSV(&t1[0], &t1[1]));
        ls_write(g, g->de[i].ds[2], 288, 0, LS_S_ZERO, LSV(&t1[0], &t1[1]), LSV(&t2[0], &t2[1]));
        ls_write(g, g->de[i].ds[3], 289, ub, LS_S_ZERO, LSV(&t2[0], &t2[1], pg, flw), LSV(&g->de[i].out1));
        ls_write(g, g->de[i].ds2[0], 294, 0, LS_S_ZERO, LSV(&t2[0]), LSV(&t1[0]));
        ls_write(g, g->de[i].ds2[1], 295, 0, LS_S_ZERO, LSV(&t1[0]), LSV(&t2[0]));
        ls_write(g, g->de[i].ds2[2], 296, 0, LS_S_ZERO, LSV(&t2[0]), LSV(&t1[0]));
        ls_write(g, g->de[i].ds2[3], 297, ub, LS_S_ZERO, LSV(&t1[0], p2), LSV(&g->de[i].out2)); }
    g->w = w; g->h = h; g->fw = fw; g->fh = fh; g->fmt = fmt; g->ufmt = u; g->flow = flow; g->fresh = 1; g->n = 0;
    return 1;
}
// First use of new images: UNDEFINED → GENERAL and cleared (the history starts black), once
static void ls_begin(lsfg *g, VkCommandBuffer cb) {
    if (!g->fresh) return;
    g->fresh = 0; g->n = 0;
    const VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }; const VkClearColorValue zero = { { 0, 0, 0, 0 } };
    for (int k = 0; k < g->nall; k++) {
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g->all[k]->img, .subresourceRange = rg };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkCmdClearColorImage(cb, g->all[k]->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg); }
}
// What the passes before wrote (and the caller's copy into c[p]) is visible to the next pass
static void ls_bar(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static int g_ls_block;   // the constants' block the passes being recorded use (0: time 0, else 1 + the generation slot)
static void ls_disp(lsfg *g, VkCommandBuffer cb, int id, VkDescriptorSet ds, const ls_img *e, uint32_t tile) {
    uint32_t off = (uint32_t)g_ls_block * 3 * g->ustride;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pl[LS_IX(id)], 0, 1, &ds, LS_CNT[LS_IX(id)][0], &off); vkCmdDispatch(cb, (e->w + tile - 1) / tile, (e->h + tile - 1) / tile, 1);
}
static void ls_run(lsfg *g, VkCommandBuffer cb, int id, VkDescriptorSet ds, const ls_img *e, uint32_t tile) {
    ls_bar(cb); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipe[LS_IX(id)]); ls_disp(g, cb, id, ds, e, tile);
}
// The frame the caller has just copied into c[p] (p swaps every frame): everything that depends on one frame alone or on the frames so far
static void ls_ingest(lsfg *g, VkCommandBuffer cb, int p) {
    int h = (int)(g->n % 3); ls_img fl = { .w = g->fw, .h = g->fh };
    g_ls_block = 0; ls_run(g, cb, 255, g->ds_mip[p], &fl, 64);
    for (int s = 0; s < 4; s++) { ls_bar(cb); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipe[LS_IX(290 + s)]);
        for (int l = 0; l < LS_MIPS; l++) ls_disp(g, cb, 290 + s, s < 3 ? g->al[l].ds[s] : g->al[l].last[h], s < 2 ? &g->al[l].t1 : &g->al[l].t3[0], 8); }
    ls_run(g, cb, 298, g->be.first[h], &g->be.t1[0], 8);
    for (int k = 0; k < 3; k++) ls_run(g, cb, 299 + k, g->be.ds[k], &g->be.t1[0], 8);
    g_ls_block = 1; ls_run(g, cb, 302, g->be.ds[3], &g->be.t1[0], 32);   // (time 0.5, as Eden's chain gives it whatever the count)
    g->p = p; g->hist = h; g->n++;
}
// Picture gen of count between the last two ingested frames (time (gen + 1) / (count + 1) from the older) → m[gen], then readable by a fragment shader or a copy.
// g->n says how many frames it has seen since its images were new
static void ls_generate(lsfg *g, VkCommandBuffer cb, int count, int gen) {
    int h = g->hist;
    if (count < 1) count = 1;
    if (count > LS_MAXGEN) count = LS_MAXGEN;
    if (gen < 0 || gen >= count) gen = 0;
    g_ls_block = 1 + (count - 1) * count / 2 + gen;
    for (int i = 0; i < LS_MIPS; i++) { const ls_img *e = &g->ga[i].t1[0];
        ls_run(g, cb, 280, g->ga[i].first[h], e, 8);
        for (int k = 0; k < 4; k++) ls_run(g, cb, 282 + k, g->ga[i].ds[k], e, 8);
        if (i < 4) continue;
        int d = i - 4; e = &g->de[d].t1[0];
        ls_run(g, cb, 280, g->de[d].first[h], e, 8);
        for (int k = 0; k < 4; k++) ls_run(g, cb, 286 + k, g->de[d].ds[k], e, 8);
        ls_run(g, cb, 281, g->de[d].sixth[h], e, 8);
        for (int k = 0; k < 4; k++) ls_run(g, cb, 294 + k, g->de[d].ds2[k], e, 8); }
    ls_run(g, cb, 256, g->ds_gen[g->p][gen], &g->m[gen], 16);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
