// Frame generation: one in-between picture from two game frames (nvn_vk.c present_window_locked shows it half a frame before the real one; DM_FRAMEGEN, off by default).
// Written for this engine from the textbook method (motion estimated from the middle, coarse to fine, on a brightness pyramid); no code from any other frame generator.
//   fg_ingest:   the frame copy c[p] → its brightness pyramid y[p][0..3] (a quarter of the frame's size, then halves): overlay/fg_luma.frag
//   fg_generate: the motion between c[p ^ 1] and c[p] from the coarsest level to the finest (overlay/fg_flow.frag), how different the two frames are as one texel
//                (fg_stat.frag), then the in-between picture m at the frame's own size (fg_gen.frag)
// It owns its images, render passes and pipelines and touches nothing of the caller's but the device, so the same code runs in the engine and in the unit test
// (tests/t_fgen_vk.c, on any Vulkan driver). Every image stays in the GENERAL layout; a memory barrier follows each pass. The caller serializes use of the queue and
// waits for the device before fg_setup changes size (it destroys the images)
#pragma once
#include <stdint.h>
#include <string.h>
#ifndef FG_PLAN_ONLY   // (tests/t_fgen.c: the decisions alone, no Vulkan)
#include "overlay/fgen_spv.h"
#define FG_LEVELS 4
#ifndef FG_MEM   // the caller's allocator (nvn_vk.c: vk_alloc) or, here, one allocation per image
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
typedef struct { VkImage img; VkImageView view; VkFramebuffer fb; fg_mem mem; int has; } fg_img;
typedef struct {
    VkDevice dev; VkPhysicalDevice pd; VkFormat fmt; uint32_t w, h, lw[FG_LEVELS], lh[FG_LEVELS]; int base, failed, fresh, debug; const char *why;
    VkSampler samp; VkRenderPass rp[3]; VkFormat rpfmt;   // [0] R8 (brightness), [1] RGBA16F (vectors), [2] the frame's format
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkDescriptorPool dp; VkPipeline pipe[4];   // luma, flow, stat, gen
    fg_img c[2], m, y[2][FG_LEVELS], f[FG_LEVELS], st;   // frame copies (previous / current, swapping), the in-between picture, brightness levels, vectors, the one texel
    VkDescriptorSet ds_luma[2][FG_LEVELS], ds_flow[2][FG_LEVELS], ds_stat, ds_gen[2];
} fgen;
static int fg_fail(fgen *g, const char *why) { g->failed = 1; g->why = why; return 0; }
static VkRenderPass fg_rp(fgen *g, VkFormat fmt) {
    VkAttachmentDescription ad = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd };
    VkRenderPass rp = VK_NULL_HANDLE; if (vkCreateRenderPass(g->dev, &ci, NULL, &rp)) return VK_NULL_HANDLE; return rp;
}
static VkPipeline fg_pipe(fgen *g, const uint32_t *frag, size_t nfrag, VkRenderPass rp) {
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE; VkPipeline p = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fg_vert_spv, .pCode = fg_vert_spv }, mf = mv; mf.codeSize = nfrag; mf.pCode = frag;
    if (vkCreateShaderModule(g->dev, &mv, NULL, &vm)) return VK_NULL_HANDLE;
    if (vkCreateShaderModule(g->dev, &mf, NULL, &fm)) { vkDestroyShaderModule(g->dev, vm, NULL); return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2] = { { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
                                              { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g->pl, .renderPass = rp };
    if (vkCreateGraphicsPipelines(g->dev, VK_NULL_HANDLE, 1, &gi, NULL, &p)) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(g->dev, vm, NULL); vkDestroyShaderModule(g->dev, fm, NULL);
    return p;
}
static void fg_img_free(fgen *g, fg_img *i) {
    if (i->fb) vkDestroyFramebuffer(g->dev, i->fb, NULL);
    if (i->view) vkDestroyImageView(g->dev, i->view, NULL);
    if (i->img) vkDestroyImage(g->dev, i->img, NULL);
    if (i->has) fg_mem_put(g->dev, i->mem);
    memset(i, 0, sizeof *i);
}
static int fg_img_make(fgen *g, fg_img *i, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkRenderPass rp) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
    if (vkCreateImage(g->dev, &ci, NULL, &i->img)) { i->img = VK_NULL_HANDLE; return 0; }
    if (!fg_mem_get(g->dev, g->pd, i->img, &i->mem)) return 0;
    i->has = 1;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(g->dev, &vci, NULL, &i->view)) { i->view = VK_NULL_HANDLE; return 0; }
    if (rp) { VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &i->view, .width = w, .height = h, .layers = 1 };
              if (vkCreateFramebuffer(g->dev, &fi, NULL, &i->fb)) { i->fb = VK_NULL_HANDLE; return 0; } }
    return 1;
}
static void fg_images_free(fgen *g) {
    for (int p = 0; p < 2; p++) { fg_img_free(g, &g->c[p]); for (int l = 0; l < FG_LEVELS; l++) fg_img_free(g, &g->y[p][l]); }
    for (int l = 0; l < FG_LEVELS; l++) fg_img_free(g, &g->f[l]);
    fg_img_free(g, &g->m); fg_img_free(g, &g->st); g->w = g->h = 0;
}
static void fg_ds_write(fgen *g, VkDescriptorSet ds, VkImageView v0, VkImageView v1, VkImageView v2, VkImageView v3) {   // (every binding gets a view: a shader that reads fewer leaves the rest alone)
    VkDescriptorImageInfo ii[4] = { { g->samp, v0, VK_IMAGE_LAYOUT_GENERAL }, { g->samp, v1, VK_IMAGE_LAYOUT_GENERAL }, { g->samp, v2, VK_IMAGE_LAYOUT_GENERAL }, { g->samp, v3, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet w[4];
    for (int i = 0; i < 4; i++) w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i] };
    vkUpdateDescriptorSets(g->dev, 4, w, 0, NULL);
}
static int fg_fmt_ok(fgen *g, VkFormat f, VkFormatFeatureFlags need) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g->pd, f, &fp); return (fp.optimalTilingFeatures & need) == need; }
// Ready for frames of this format and size (the game frame's crop): 1, or 0 with g->why once it cannot run at all (g->failed: the caller stops asking).
// A new size or format destroys the images: the device must be idle
static int fg_setup(fgen *g, VkDevice dev, VkPhysicalDevice pd, VkFormat fmt, uint32_t w, uint32_t h) {
    if (g->failed) return 0;
    if (g->w == w && g->h == h && g->fmt == fmt) return 1;
    g->dev = dev; g->pd = pd;
    const VkFormatFeatureFlags tgt = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!g->base) {
        if (!fg_fmt_ok(g, VK_FORMAT_R8_UNORM, tgt) || !fg_fmt_ok(g, VK_FORMAT_R16G16B16A16_SFLOAT, tgt)) return fg_fail(g, "the driver cannot render to and filter R8 / RGBA16F");
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f };
        VkDescriptorSetLayoutBinding b[4]; for (int i = 0; i < 4; i++) b[i] = (VkDescriptorSetLayoutBinding){ (uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b };
        if (vkCreateSampler(dev, &sci, NULL, &g->samp) || vkCreateDescriptorSetLayout(dev, &dli, NULL, &g->dsl)) return fg_fail(g, "no sampler / descriptor layout");
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g->dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
        if (vkCreatePipelineLayout(dev, &pli, NULL, &g->pl)) return fg_fail(g, "no pipeline layout");
        enum { NSET = 4 * FG_LEVELS + 3 };
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSET * 4 };
        VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = NSET, .poolSizeCount = 1, .pPoolSizes = &ps };
        if (vkCreateDescriptorPool(dev, &dpi, NULL, &g->dp)) return fg_fail(g, "no descriptor pool");
        VkDescriptorSet all[NSET]; VkDescriptorSetLayout ls[NSET]; for (int i = 0; i < NSET; i++) ls[i] = g->dsl;
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g->dp, .descriptorSetCount = NSET, .pSetLayouts = ls };
        if (vkAllocateDescriptorSets(dev, &dai, all)) return fg_fail(g, "no descriptor sets");
        int k = 0; for (int p = 0; p < 2; p++) for (int l = 0; l < FG_LEVELS; l++) { g->ds_luma[p][l] = all[k++]; g->ds_flow[p][l] = all[k++]; }
        g->ds_stat = all[k++]; g->ds_gen[0] = all[k++]; g->ds_gen[1] = all[k++];
        g->rp[0] = fg_rp(g, VK_FORMAT_R8_UNORM); g->rp[1] = fg_rp(g, VK_FORMAT_R16G16B16A16_SFLOAT);
        if (!g->rp[0] || !g->rp[1]) return fg_fail(g, "no render pass");
        g->pipe[0] = fg_pipe(g, fg_luma_frag_spv, sizeof fg_luma_frag_spv, g->rp[0]); g->pipe[1] = fg_pipe(g, fg_flow_frag_spv, sizeof fg_flow_frag_spv, g->rp[1]);
        g->pipe[2] = fg_pipe(g, fg_stat_frag_spv, sizeof fg_stat_frag_spv, g->rp[1]);
        if (!g->pipe[0] || !g->pipe[1] || !g->pipe[2]) return fg_fail(g, "a pipeline failed");
        g->base = 1;
    }
    if (w < 64 || h < 64) { g->why = "the frame is too small"; return 0; }
    if (!fg_fmt_ok(g, fmt, tgt)) return fg_fail(g, "the driver cannot render to and filter the frame's format");
    fg_images_free(g);
    if (g->rpfmt != fmt) {
        if (g->pipe[3]) { vkDestroyPipeline(dev, g->pipe[3], NULL); g->pipe[3] = VK_NULL_HANDLE; }
        if (g->rp[2]) { vkDestroyRenderPass(dev, g->rp[2], NULL); g->rp[2] = VK_NULL_HANDLE; }
        g->rp[2] = fg_rp(g, fmt); if (!g->rp[2]) return fg_fail(g, "no render pass for the frame's format");
        g->pipe[3] = fg_pipe(g, fg_gen_frag_spv, sizeof fg_gen_frag_spv, g->rp[2]); if (!g->pipe[3]) return fg_fail(g, "the picture's pipeline failed");
        g->rpfmt = fmt;
    }
    g->lw[0] = (w + 3) / 4; g->lh[0] = (h + 3) / 4;
    for (int l = 1; l < FG_LEVELS; l++) { g->lw[l] = (g->lw[l - 1] + 1) / 2; g->lh[l] = (g->lh[l - 1] + 1) / 2; }
    const VkImageUsageFlags smp = VK_IMAGE_USAGE_SAMPLED_BIT, att = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    int ok = fg_img_make(g, &g->m, fmt, w, h, att | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g->rp[2]) && fg_img_make(g, &g->st, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, att, g->rp[1]);
    for (int p = 0; p < 2 && ok; p++) { ok = fg_img_make(g, &g->c[p], fmt, w, h, smp | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_NULL_HANDLE);
        for (int l = 0; l < FG_LEVELS && ok; l++) ok = fg_img_make(g, &g->y[p][l], VK_FORMAT_R8_UNORM, g->lw[l], g->lh[l], att, g->rp[0]); }
    for (int l = 0; l < FG_LEVELS && ok; l++) ok = fg_img_make(g, &g->f[l], VK_FORMAT_R16G16B16A16_SFLOAT, g->lw[l], g->lh[l], att, g->rp[1]);
    if (!ok) { fg_images_free(g); return fg_fail(g, "no memory for its images"); }
    for (int p = 0; p < 2; p++) {
        for (int l = 0; l < FG_LEVELS; l++) {
            VkImageView src = l ? g->y[p][l - 1].view : g->c[p].view;
            fg_ds_write(g, g->ds_luma[p][l], src, src, src, src);
            VkImageView a = g->y[p ^ 1][l].view, b = g->y[p][l].view, c = l + 1 < FG_LEVELS ? g->f[l + 1].view : b;
            fg_ds_write(g, g->ds_flow[p][l], a, b, c, b); }
        fg_ds_write(g, g->ds_gen[p], g->c[p ^ 1].view, g->c[p].view, g->f[0].view, g->st.view); }
    { VkImageView v = g->f[0].view; fg_ds_write(g, g->ds_stat, v, v, v, v); }
    g->w = w; g->h = h; g->fmt = fmt; g->fresh = 1;
    return 1;
}
// First use of new images: UNDEFINED → GENERAL, once
static void fg_begin(fgen *g, VkCommandBuffer cb) {
    if (!g->fresh) return;
    VkImageMemoryBarrier ib[2 + 2 * FG_LEVELS + FG_LEVELS + 2]; int n = 0; VkImage im[2 + 2 * FG_LEVELS + FG_LEVELS + 2];
    im[n++] = g->m.img; im[n++] = g->st.img;
    for (int p = 0; p < 2; p++) { im[n++] = g->c[p].img; for (int l = 0; l < FG_LEVELS; l++) im[n++] = g->y[p][l].img; }
    for (int l = 0; l < FG_LEVELS; l++) im[n++] = g->f[l].img;
    for (int i = 0; i < n; i++) ib[i] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, (uint32_t)n, ib);
    g->fresh = 0;
}
static void fg_pass(fgen *g, VkCommandBuffer cb, int rp, fg_img *dst, uint32_t w, uint32_t h, int pipe, VkDescriptorSet ds, float a, float b, float c, float d) {
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g->rp[rp], .framebuffer = dst->fb, .renderArea = { { 0, 0 }, { w, h } } };
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v = { 0, 0, (float)w, (float)h, 0, 1 }; VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipe[pipe]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &ds, 0, NULL);
    float pc[4] = { a, b, c, d }; vkCmdPushConstants(cb, g->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// The frame the caller copied into c[p] (and made readable): its brightness pyramid
static void fg_ingest(fgen *g, VkCommandBuffer cb, int p) {
    fg_begin(g, cb);
    fg_pass(g, cb, 0, &g->y[p][0], g->lw[0], g->lh[0], 0, g->ds_luma[p][0], 1.0f / g->w, 1.0f / g->h, 1, 0);
    for (int l = 1; l < FG_LEVELS; l++) fg_pass(g, cb, 0, &g->y[p][l], g->lw[l], g->lh[l], 0, g->ds_luma[p][l], 0.5f / g->lw[l - 1], 0.5f / g->lh[l - 1], 0, 0);
}
// The picture between c[p ^ 1] (the frame before, ingested then) and c[p] (ingested now) into m
static void fg_generate(fgen *g, VkCommandBuffer cb, int p) {
    for (int l = FG_LEVELS - 1; l >= 0; l--)   // the coarsest level looks 4 texels each way (128 pixels of a 1080p frame), the others refine by one
        fg_pass(g, cb, 1, &g->f[l], g->lw[l], g->lh[l], 1, g->ds_flow[p][l], 1.0f / g->lw[l], 1.0f / g->lh[l], l == FG_LEVELS - 1 ? 4 : 1, l == FG_LEVELS - 1 ? 0 : 1);
    fg_pass(g, cb, 1, &g->st, 1, 1, 2, g->ds_stat, 0, 0, 0, 0);
    fg_pass(g, cb, 2, &g->m, g->w, g->h, 3, g->ds_gen[p], 1.0f / g->lw[0], 1.0f / g->lh[0], g->debug ? 1 : 0, 0);
}
#endif
// ---------- When to generate, and when the real frame goes out (pure: tests/t_fgen.c) ----------
typedef struct { uint64_t last, cost_sum, cost_n, cost_max, win; int over; } fg_plan;
// A real frame arrived at now (ns); period: the game's frame time. 1: make an in-between picture (the frame before came about one period ago: after a pause, a
// loading hitch or the first frame there is nothing sensible to put between)
static inline int fg_plan_frame(fg_plan *s, uint64_t now, uint64_t period) {
    uint64_t d = s->last ? now - s->last : 0; s->last = now;
    return d && d * 2 >= period && d * 2 <= period * 3;
}
// When the real frame is handed to the display after its in-between picture. No display times asked (want 0), or a display whose refresh is the half period (60 Hz):
// half a period after the in-between. With asked times (want: the in-between's; the real frame then asks want + half): early hundredths of a refresh before want.
// By then the display has taken the in-between (handed over sooner, the real frame would replace it under mailbox) and the real frame still has refreshes in hand
// to make its own time (handed over later, it shows a refresh late)
static inline uint64_t fg_real_due(uint64_t now, uint64_t want, uint64_t half, uint64_t refresh, int early) {
    if (!want || !refresh || half < 2 * refresh) return now + half;
    uint64_t due = want - (uint64_t)((int64_t)refresh * early / 100); return due > now ? due : now;
}
// The GPU's time for one in-between picture (ns). Returns 1 once the mean over a 2 s window is over the budget twice running: too slow, the caller turns it off
static inline int fg_plan_cost(fg_plan *s, uint64_t gpu_ns, uint64_t now, uint64_t budget) {
    if (!s->win) s->win = now;
    s->cost_sum += gpu_ns; s->cost_n++; if (gpu_ns > s->cost_max) s->cost_max = gpu_ns;
    if (now - s->win < 2000000000ull || s->cost_n < 16) return 0;
    int slow = s->cost_sum / s->cost_n > budget; s->over = slow ? s->over + 1 : 0;
    s->cost_sum = s->cost_n = s->cost_max = 0; s->win = now;
    return s->over >= 2;
}
