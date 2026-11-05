// bc_gpu.comp against the CPU decoder / transcoder (../bc_decode.c, ../etc_encode.c) on any Vulkan device (lavapipe will do): random blocks of every kind, and
// smooth gradients encoded as BC1 / BC4 by hand, decoded (mode 0) and transcoded (mode 1). Decoding, ETC2, EAC and BC6H → B10G11R11 must match bit for bit;
// ASTC (floats on the GPU, doubles on the CPU) must decode to within 2% more squared error than the CPU's block.
// Build: cc -O2 -I.. -I<vulkan headers> bc_gpu_check.c ../bc_decode.c ../etc_encode.c -l:libvulkan.so.1 -lpthread -lm; run with the path of bc_gpu.comp.spv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "bc_decode.h"
#include "etc_encode.h"
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool cp; static VkShaderModule smod; static VkPipelineLayout pl; static VkDescriptorSet ds;
static VkBuffer bin, bout; static VkDeviceMemory min_, mout; static uint8_t *pin, *pout; enum { CAP = 1 << 22 };
typedef struct { int32_t kind; uint32_t mode, bw, bh, nz, in_off, in_row, in_slice, out_off, out_row, out_slice, w, h; } pcs;
static void buf(VkBuffer *b, VkDeviceMemory *m, uint8_t **p) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = CAP, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }; CK(vkCreateBuffer(dev, &bi, NULL, b));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, *b, &rq); VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t t = 0; for (; t < mp.memoryTypeCount; t++) if ((rq.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & 6) == 6) break;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t }; CK(vkAllocateMemory(dev, &ai, NULL, m));
    CK(vkBindBufferMemory(dev, *b, *m, 0)); CK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, (void **)p));
}
static void init(const char *spv) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance in; CK(vkCreateInstance(&ii, NULL, &in));
    uint32_t n = 1; vkEnumeratePhysicalDevices(in, &n, &pd); VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd, &pr); printf("device: %s\n", pr.deviceName);
    float pri = 1; VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &pri };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi }; CK(vkCreateDevice(pd, &di, NULL, &dev));
    vkGetDeviceQueue(dev, 0, 0, &q);
    FILE *f = fopen(spv, "rb"); if (!f) { perror(spv); exit(1); } fseek(f, 0, 2); long sz = ftell(f); rewind(f); uint32_t *code = malloc(sz); fread(code, 1, sz, f); fclose(f);
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code }; VkShaderModule sm; CK(vkCreateShaderModule(dev, &mi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb }; VkDescriptorSetLayout dl; CK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dl));
    VkPushConstantRange rg = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcs) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl, .pushConstantRangeCount = 1, .pPushConstantRanges = &rg }; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    smod = sm;   // (a pipeline per kind and mode: run(), the shader specialized as bc_gpu.c does)
    buf(&bin, &min_, &pin); buf(&bout, &mout, &pout);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }; VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl }; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo b0 = { bin, 0, VK_WHOLE_SIZE }, b1 = { bout, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b0 },
                                  { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b1 } };
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT }; CK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
}
static void run(pcs p) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
    int32_t sc[2] = { p.kind, (int32_t)p.mode }; VkSpecializationMapEntry me[2] = { { 0, 0, 4 }, { 1, 4, 4 } }; VkSpecializationInfo spi = { 2, me, sizeof sc, sc };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = smod, .pName = "main", .pSpecializationInfo = &spi }, .layout = pl };
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p); vkCmdDispatch(cb, (p.bw + 7) / 8, (p.bh + 7) / 8, p.nz); vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }; CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); vkQueueWaitIdle(q);
    vkFreeCommandBuffers(dev, cp, 1, &cb);
}
static uint32_t rs = 12345; static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
// a squared error of an ASTC block, decoded by the independent reference in etc_check.py's rules: here only relative, against the CPU encoder's own block
static long astc_err(const uint8_t blk[16], const uint8_t px[16][4]);
int main(int argc, char **argv) {
    init(argc > 1 ? argv[1] : "bc_gpu.comp.spv");
    static const int kinds[] = { 1, 2, 3, 4, -4, 5, -5, 6, -6, 7 }; int bad = 0;
    for (unsigned kk = 0; kk < sizeof kinds / sizeof *kinds; kk++) {
        int kind = kinds[kk], bb = bc_block_bytes(kind), tb = bc_texel_bytes(kind); const int bw = 64, bh = 32, w = bw * 4 - 1, h = bh * 4 - 2;   // (edge blocks cut)
        size_t ip = (size_t)bw * bb; uint8_t *src = pin;
        for (size_t i = 0; i < ip * bh; i++) src[i] = (uint8_t)rnd();
        // half the rows: smooth content (endpoints close together, indices ordered) so the encoders meet ordinary blocks, not only noise
        for (int by = 0; by < bh / 2; by++) for (int bx = 0; bx < bw; bx++) { uint8_t *b = src + by * ip + bx * bb; if (kind == 1 || kind == 2 || kind == 3) { uint8_t *c = b + (kind == 1 ? 0 : 8); c[1] = c[0] ^ 0x08; } if (kind == 4 || kind == 5 || kind == 3) b[1] = b[0] + 3; }
        // decode
        size_t orow = (size_t)bw * 4 * tb; pcs p = { kind, 0, bw, bh, 1, 0, (uint32_t)(ip / 4), 0, 0, (uint32_t)(orow / 4), 0, w, h };
        memset(pout, 0xcd, orow * bh * 4); run(p);
        uint8_t *cpu = malloc(orow * bh * 4); bc_decode_image(kind, src, ip, bw * 4, bh * 4, cpu, orow);
        long dm = 0; for (size_t i = 0; i < orow * bh * 4; i++) dm += cpu[i] != pout[i];
        printf("kind %2d decode: %ld of %zu bytes differ\n", kind, dm, orow * bh * 4); bad |= dm != 0;
        // transcode
        if (!bc_transcode_vk(kind, 0, 1) || kind == -6) { free(cpu); continue; }
        int ob = bc_transcode_bytes(kind), blocks = bc_transcode_blocks(kind); size_t trow = blocks ? (size_t)bw * ob : (size_t)w * 4, tsz = blocks ? trow * bh : trow * h;
        uint8_t *ct = calloc(1, tsz); bc_transcode_image(kind, src, ip, w, h, ct, trow);
        pcs t = { kind, 1, bw, bh, 1, 0, (uint32_t)(ip / 4), 0, 0, (uint32_t)(trow / 4), 0, w, h }; memset(pout, 0, tsz); run(t);
        if (kind != 7) { long tm = 0; for (size_t i = 0; i < tsz; i++) tm += ct[i] != pout[i]; printf("kind %2d transcode: %ld of %zu bytes differ\n", kind, tm, tsz); bad |= tm != 0; }
        else {   // ASTC: per block, the GPU's squared error against the CPU's
            long ec = 0, eg = 0; int same = 0;
            for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
                uint8_t px[16][4]; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) memcpy(px[y * 4 + x], cpu + (by * 4 + y) * orow + (bx * 4 + x) * 4, 4);
                const uint8_t *a = ct + by * trow + bx * 16, *g = pout + by * trow + bx * 16; ec += astc_err(a, px); eg += astc_err(g, px); same += !memcmp(a, g, 16);
            }
            printf("kind  7 transcode (ASTC): %d of %d blocks identical; squared error CPU %ld, GPU %ld (%+.2f%%)\n", same, bw * bh, ec, eg, ec ? 100.0 * (eg - ec) / ec : 0.0);
            bad |= ec && eg > ec + ec / 50;
        }
        free(ct); free(cpu);
    }
    puts(bad ? "FAILED" : "all match"); return bad;
}
// ASTC 4x4 decode of the two block shapes the encoder writes (etc_encode.c astc_encode_rgba): CEM 8 with 8 weight levels, CEM 12 with 6 (trits)
static int bget(const uint8_t *b, int pos, int n, int rev) { int v = 0; for (int i = 0; i < n; i++) { int p = rev ? 127 - (pos + i) : pos + i; v |= (b[p >> 3] >> (p & 7) & 1) << i; } return v; }
static long astc_err(const uint8_t blk[16], const uint8_t px[16][4]) {
    int mode = bget(blk, 0, 11, 0), alpha = mode == 0x43, nc = alpha ? 4 : 3, e[2][4], w[16];
    static const int W8[8] = { 0, 9, 18, 27, 37, 46, 55, 64 }, W6[6] = { 0, 64, 12, 52, 25, 39 };
    for (int c = 0; c < 4; c++) { e[0][c] = c < nc ? bget(blk, 17 + 16 * c, 8, 0) : 255; e[1][c] = c < nc ? bget(blk, 25 + 16 * c, 8, 0) : 255; }
    if (!alpha) for (int i = 0; i < 16; i++) w[i] = W8[bget(blk, 3 * i, 3, 1)];
    else {
        static const int seq[10][2] = { { -1, 0 }, { 0, 2 }, { -1, 1 }, { 2, 2 }, { -1, 2 }, { 4, 1 }, { -1, 3 }, { 5, 2 }, { -1, 4 }, { 7, 1 } };
        for (int g = 0, pos = 0; g < 4; g++) {
            int T = 0, m[5] = { 0 };
            for (int q = 0; q < 10; q++) { if (seq[q][0] < 0) { if (pos < 42) m[seq[q][1]] = bget(blk, pos, 1, 1); pos++; } else { int n = seq[q][1]; for (int i = 0; i < n; i++, pos++) if (pos < 42) T |= bget(blk, pos, 1, 1) << (seq[q][0] + i); } }
            int t[5], C;   // the specification's trit decode
            if (((T >> 2) & 7) == 7) { C = ((T >> 5) & 7) << 2 | (T & 3); t[4] = t[3] = 2; }
            else { C = T & 31; if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = T >> 7 & 1; } else { t[4] = T >> 7 & 1; t[3] = T >> 5 & 3; } }
            if ((C & 3) == 3) { t[2] = 2; t[1] = C >> 4 & 1; t[0] = (C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1); }
            else if (((C >> 2) & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
            else { t[2] = C >> 4 & 1; t[1] = C >> 2 & 3; t[0] = (C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1); }
            for (int j = 0; j < 5 && g * 5 + j < 16; j++) w[g * 5 + j] = W6[t[j] * 2 + m[j]];
        }
    }
    if (e[1][0] + e[1][1] + e[1][2] < e[0][0] + e[0][1] + e[0][2]) { for (int c = 0; c < 4; c++) { int s = e[0][c]; e[0][c] = e[1][c]; e[1][c] = s; } }   // (blue contraction order: the decoder swaps)
    long err = 0;
    for (int i = 0; i < 16; i++) for (int c = 0; c < nc; c++) { int got = ((e[0][c] * 257) * (64 - w[i]) + (e[1][c] * 257) * w[i] + 32) >> 14, d = got - px[i][c]; err += d * d; }
    return err;
}
