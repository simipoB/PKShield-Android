// The BC layer (../bc_gpu.c) end to end on any Vulkan device (lavapipe will do): bc_gpu_init's own check, then for every kind and mode the device's format can
// hold, blocks converted into a real image through bc_gpu_buffer_to_image and through bc_gpu_image_to_image (from a uint image holding the blocks, as a game
// writes them), read back and compared with the CPU's conversion; edge sizes (not a multiple of 4) and two array layers.
// Build: cc -O2 -I.. -I<vulkan headers> bc_gpu_layer_check.c ../bc_gpu.c ../bc_decode.c ../etc_encode.c -l:libvulkan.so.1 -lpthread -lm
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_gpu.h"
#include "bc_decode.h"
#include "etc_encode.h"
extern PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *);   // (the loader's; bc_gpu.h asks for no prototypes)
static PFN_vkGetInstanceProcAddr gipa;
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)
#define F(f) static PFN_##f f;
F(vkCreateInstance) F(vkGetPhysicalDeviceFeatures) F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceFormatProperties) F(vkCreateDevice) F(vkGetDeviceProcAddr)
F(vkGetDeviceQueue) F(vkCreateImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkBindImageMemory) F(vkCreateBuffer) F(vkGetBufferMemoryRequirements) F(vkBindBufferMemory)
F(vkMapMemory) F(vkCreateCommandPool) F(vkAllocateCommandBuffers) F(vkBeginCommandBuffer) F(vkEndCommandBuffer) F(vkQueueSubmit) F(vkQueueWaitIdle) F(vkCmdPipelineBarrier)
F(vkCmdCopyBufferToImage) F(vkCmdCopyImageToBuffer) F(vkDestroyImage) F(vkDestroyBuffer) F(vkFreeMemory)
static VkDevice dev; static VkQueue q; static VkCommandPool cp; static VkPhysicalDeviceMemoryProperties mp; static VkPhysicalDevice pd;
static void lg(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); puts(""); }
static uint32_t mt(uint32_t bits, VkMemoryPropertyFlags w) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & w) == w) return i; return 0; }
static VkBuffer hbuf(VkDeviceSize n, uint8_t **p) { VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer b; CK(vkCreateBuffer(dev, &bi, NULL, &b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, b, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mt(rq.memoryTypeBits, 6) }; VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m));
    CK(vkBindBufferMemory(dev, b, m, 0)); CK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)p)); return b; }
static VkImage mkimg(VkFormat f, uint32_t w, uint32_t h, uint32_t layers) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = layers, .samples = 1,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    VkImage im; CK(vkCreateImage(dev, &ci, NULL, &im)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev, im, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mt(rq.memoryTypeBits, 0) }; VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m));
    CK(vkBindImageMemory(dev, im, m, 0)); return im; }
static VkCommandBuffer begin(void) { VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = 0, .commandBufferCount = 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi); return cb; }
static void end(VkCommandBuffer cb) { vkEndCommandBuffer(cb); VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }; CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); vkQueueWaitIdle(q); }
static void to_general(VkCommandBuffer cb, VkImage im) { VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, .image = im,
    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS } }; vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b); }
static uint32_t rs = 777; static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
int main(void) {
#define G(f) f = (PFN_##f)gipa(in, #f);
    gipa = vkGetInstanceProcAddr; VkInstance in = NULL; G(vkCreateInstance)
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 }; VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CK(vkCreateInstance(&ii, NULL, &in));
    G(vkEnumeratePhysicalDevices) G(vkGetPhysicalDeviceFeatures) G(vkGetPhysicalDeviceMemoryProperties) G(vkGetPhysicalDeviceFormatProperties) G(vkCreateDevice) G(vkGetDeviceProcAddr)
    uint32_t n = 1; vkEnumeratePhysicalDevices(in, &n, &pd); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float pri = 1; VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &pri };
    VkPhysicalDeviceFeatures has; vkGetPhysicalDeviceFeatures(pd, &has);   // (lavapipe: what it has; formats it lacks are skipped below)
    VkPhysicalDeviceFeatures ft = { .textureCompressionETC2 = has.textureCompressionETC2, .textureCompressionASTC_LDR = has.textureCompressionASTC_LDR };
    printf("ETC2 %d, ASTC %d\n", has.textureCompressionETC2, has.textureCompressionASTC_LDR);
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .pEnabledFeatures = &ft }; CK(vkCreateDevice(pd, &di, NULL, &dev));
#define D(f) f = (PFN_##f)vkGetDeviceProcAddr(dev, #f);
    D(vkGetDeviceQueue) D(vkCreateImage) D(vkGetImageMemoryRequirements) D(vkAllocateMemory) D(vkBindImageMemory) D(vkCreateBuffer) D(vkGetBufferMemoryRequirements) D(vkBindBufferMemory)
    D(vkMapMemory) D(vkCreateCommandPool) D(vkAllocateCommandBuffers) D(vkBeginCommandBuffer) D(vkEndCommandBuffer) D(vkQueueSubmit) D(vkQueueWaitIdle) D(vkCmdPipelineBarrier)
    D(vkCmdCopyBufferToImage) D(vkCmdCopyImageToBuffer) D(vkDestroyImage) D(vkDestroyBuffer) D(vkFreeMemory)
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; CK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    bc_gpu_info bi = { dev, q, 0, vkGetDeviceProcAddr, &mp, lg }; if (!bc_gpu_init(&bi)) { puts("FAILED: layer off"); return 1; }
    // (kind, mode) → the image format the engine makes (nvn_fmt_vk.c): decoded RGBA8 / R8 / RG8 (snorm for signed) / RGBA16F; transcoded ETC2 / EAC / B10G11R11 / ASTC
    static const struct { int kind, mode; VkFormat f; uint32_t tb; } C[] = {
        { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 4 }, { 3, 0, VK_FORMAT_R8G8B8A8_UNORM, 4 }, { 4, 0, VK_FORMAT_R8_UNORM, 1 }, { -5, 0, VK_FORMAT_R8G8_SNORM, 2 }, { 6, 0, VK_FORMAT_R16G16B16A16_SFLOAT, 8 },
        { 7, 0, VK_FORMAT_R8G8B8A8_UNORM, 4 }, { 1, 1, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 16 }, { 2, 1, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 16 }, { 3, 1, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 16 },
        { 4, 1, VK_FORMAT_EAC_R11_UNORM_BLOCK, 8 }, { -4, 1, VK_FORMAT_EAC_R11_SNORM_BLOCK, 8 }, { 5, 1, VK_FORMAT_EAC_R11G11_UNORM_BLOCK, 16 }, { -5, 1, VK_FORMAT_EAC_R11G11_SNORM_BLOCK, 16 },
        { 6, 1, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4 }, { 7, 1, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 16 } };
    int bad = 0; uint8_t *hp; VkBuffer hb = hbuf(8 << 20, &hp);
    for (unsigned i = 0; i < sizeof C / sizeof *C; i++) {
        int kind = C[i].kind, mode = C[i].mode, blk = mode && kind != 6; const uint32_t w = 61, h = 30, L = 2, bw = (w + 3) / 4, bh = (h + 3) / 4, bb = (uint32_t)bc_block_bytes(kind);
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, C[i].f, &fp); if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) { printf("kind %2d mode %d: format %d not on this device, skipped\n", kind, mode, C[i].f); continue; }
        uint32_t ip = bw * bb, isl = ip * bh; uint8_t *src = hp; for (uint32_t k = 0; k < isl * L; k++) src[k] = (uint8_t)rnd();
        // the CPU's result, per layer, in the image's layout (rows of whole blocks / texels)
        size_t orow = blk ? (size_t)bw * C[i].tb : (size_t)w * C[i].tb, osl = blk ? orow * bh : orow * h; uint8_t *cpu = calloc(L, osl);
        for (uint32_t l = 0; l < L; l++) (mode ? bc_transcode_image : bc_decode_image)(kind, src + l * isl, ip, (int)w, (int)h, cpu + l * osl, orow);
        for (int path = 0; path < 2; path++) {
            VkImage im = mkimg(C[i].f, w, h, L), raw = VK_NULL_HANDLE; VkCommandBuffer cb = begin(); to_general(cb, im);
            VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, L }; int ok;
            if (!path) ok = bc_gpu_buffer_to_image(cb, kind, mode, hb, 0, ip, isl, w, h, L, im, sub, (VkOffset3D){0, 0, 0}, 0);
            else {   // the blocks in a uint image first (RG32UI for 8-byte blocks, RGBA32UI for 16), as a game's GPU writes them
                raw = mkimg(bb == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT, bw, bh, L); to_general(cb, raw);
                VkBufferImageCopy r = { .bufferRowLength = bw, .bufferImageHeight = bh, .imageSubresource = sub, .imageExtent = { bw, bh, 1 } }; vkCmdCopyBufferToImage(cb, hb, raw, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
                VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                ok = bc_gpu_image_to_image(cb, kind, mode, raw, sub, (VkOffset3D){0, 0, 0}, bw, bh, L, w, h, im, sub, (VkOffset3D){0, 0, 0}, 0);
            }
            uint8_t *rb = hp + (4 << 20); VkBufferImageCopy r = { .bufferOffset = 4 << 20, .bufferRowLength = blk ? bw * 4 : w, .bufferImageHeight = blk ? bh * 4 : h, .imageSubresource = sub, .imageExtent = { w, h, 1 } };
            memset(rb, 0xcd, osl * L); vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_GENERAL, hb, 1, &r); end(cb);
            long diff = ok ? 0 : -1; if (ok) for (size_t k = 0; k < osl * L; k++) diff += cpu[k] != rb[k];
            printf("kind %2d %-9s from a %s: %s\n", kind, mode ? "transcode" : "decode", path ? "uint image" : "buffer", !ok ? "NOT DONE" : diff ? "DIFFERS" : "same as the CPU");
            bad |= diff != 0;
            vkDestroyImage(dev, im, NULL); if (raw) vkDestroyImage(dev, raw, NULL);
        }
        free(cpu);
    }
    puts(bad ? "FAILED" : "all match"); return bad;
}
