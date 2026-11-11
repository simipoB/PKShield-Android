// SPDX-License-Identifier: GPL-3.0-or-later
// nvn_lsfg.h on a real Vulkan driver (Mesa's lavapipe on a PC is enough), with the player's Lossless Scaling shaders converted to SPIR-V (<id>.spv in a folder that
// stays outside the repository: no shader is ever part of it). A panning scene is fed frame by frame; the picture between the last two is read back and compared
// with the true one and with a plain mix of the two.
//   make t_lsfg_vk VULKAN_INC=<a folder holding vulkan/vulkan.h>     ./t_lsfg_vk <folder of .spv> [folder for .ppm pictures]
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "../nvn/nvn_lsfg.h"
#define W 1280
#define H 720
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool pool; static lsfg G;
#define NB (1 + LS_MAXGEN)
static VkBuffer buf[NB]; static VkDeviceMemory bmem[NB]; static uint8_t *map[NB];   // the frame going in, the pictures coming out
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static float hash(int x, int y, int s) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u; h = (h ^ (h >> 13)) * 1274126177u; return ((h ^ (h >> 16)) & 0xffff) / 65535.0f; }
static float vnoise(float x, float y, int s) { int xi = (int)floorf(x), yi = (int)floorf(y); float fx = x - xi, fy = y - yi; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    return (hash(xi, yi, s) * (1 - fx) + hash(xi + 1, yi, s) * fx) * (1 - fy) + (hash(xi, yi + 1, s) * (1 - fx) + hash(xi + 1, yi + 1, s) * fx) * fy; }
static void scene(float x, float y, int seed, float *rgb) {   // detail at several sizes, defined at any real position so a shifted frame is exact
    for (int c = 0; c < 3; c++) { float v = 0.5f * vnoise(x / 97, y / 97, seed + c) + 0.3f * vnoise(x / 23, y / 23, seed + 7 + c) + 0.2f * vnoise(x / 6, y / 6, seed + 13 + c);
        if (vnoise(x / 41, y / 41, seed + 31) > 0.62f) v = 1 - v;
        rgb[c] = v; }
}
static float PX, PY;
static void fill(uint8_t *m, float t) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float c[3]; scene(x + PX * t, y + PY * t, 1, c);
    for (int k = 0; k < 3; k++) m[(y * W + x) * 4 + k] = (uint8_t)(fminf(fmaxf(c[k], 0), 1) * 255 + 0.5f);
    m[(y * W + x) * 4 + 3] = 255; } }
static VkCommandBuffer cb_begin(void) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &ai, &cb); VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static double cb_end(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb); VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    VkResult r = vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE); if (!r) r = vkQueueWaitIdle(q); clock_gettime(CLOCK_MONOTONIC, &b);
    if (r) { printf("FAIL: the queue returned %d\n", r); exit(1); }
    vkFreeCommandBuffers(dev, pool, 1, &cb); return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}
static const VkBufferImageCopy BC = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
static int frame_no;
static double feed(float t, int count) {   // frame t into the chain, then (count) the pictures between it and the frame before → map[1..count]
    fill(map[0], t);
    VkCommandBuffer cb = cb_begin(); ls_begin(&G, cb); int p = frame_no++ & 1;
    vkCmdCopyBufferToImage(cb, buf[0], G.c[p].img, VK_IMAGE_LAYOUT_GENERAL, 1, &BC);
    ls_ingest(&G, cb, p);
    for (int k = 0; k < count; k++) { ls_generate(&G, cb, count, k); vkCmdCopyImageToBuffer(cb, G.m[k].img, VK_IMAGE_LAYOUT_GENERAL, buf[1 + k], 1, &BC); }
    return cb_end(cb);
}
static double err(const uint8_t *got, const uint8_t *want, int m) {
    double s = 0; int n = 0; for (int y = m; y < H - m; y++) for (int x = m; x < W - m; x++) for (int k = 0; k < 3; k++) { s += abs(got[(y * W + x) * 4 + k] - want[(y * W + x) * 4 + k]); n++; }
    return s / n;
}
static void ppm(const char *dir, const char *name, const uint8_t *m) { if (!dir) return; char p[1024]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name); FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6 %d %d 255\n", W, H); for (int i = 0; i < W * H; i++) fwrite(m + i * 4, 1, 3, f); fclose(f); }
static uint8_t before[W * H * 4], after[W * H * 4], truth[W * H * 4], blend[W * H * 4];
int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: t_lsfg_vk <folder of the converted .spv shaders> [folder for .ppm pictures]\n"); return 2; }
    const char *dir = argc > 2 ? argv[2] : NULL;
    // (a check that the loader refuses a shader in another one's place, before anything else)
    if (!ls_load(&G, argv[1])) { printf("SKIP: %s\n", G.why); return 0; }
    { const char *bad = ls_spv_check(G.spv[LS_IX(290)], G.nspv[LS_IX(290)] / 4, LS_IX(256)); CHECK(bad != NULL, "shader 290 passed as 256"); printf("290 in 256's place: refused (%s)\n", bad ? bad : "-"); }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("SKIP: no Vulkan 1.3 instance on this machine\n"); return 0; }
    uint32_t n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds); if (!n) { printf("SKIP: no Vulkan device on this machine\n"); return 0; }
    pd = pds[0]; VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    const char *lack = ls_device_lacks(pd, 0); if (lack) { printf("SKIP: %s lacks %s\n", pp.deviceName, lack); return 0; }
    if (ls_device_lacks(pd, 1)) printf("(%s lacks %s: the shaders go without their float controls)\n", pp.deviceName, ls_device_lacks(pd, 1));
    uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp); uint32_t qf = 0; while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT)) qf++;
    float prio = 1; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    // every feature the device has, on (as nvn_vk.c creates its device)
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 }; vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("SKIP: no Vulkan device could be opened\n"); return 0; }
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf }; vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (int i = 0; i < NB; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT }; vkCreateBuffer(dev, &bci, NULL, &buf[i]);
        VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, buf[i], &rq); uint32_t t = 0; const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (t < mp.memoryTypeCount && !((rq.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & hv) == hv)) t++;
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t }; vkAllocateMemory(dev, &mai, NULL, &bmem[i]);
        vkBindBufferMemory(dev, buf[i], bmem[i], 0); void *m; vkMapMemory(dev, bmem[i], 0, VK_WHOLE_SIZE, 0, &m); map[i] = m; }
    float flow = argc > 3 ? (float)atof(argv[3]) : 1.0f;
    if (!ls_setup(&G, dev, pd, VK_FORMAT_R8G8B8A8_UNORM, W, H, flow)) { printf("FAIL: cannot set up on %s: %s\n", pp.deviceName, G.why ? G.why : "?"); return 1; }
    printf("LSFG chain on %s: %dx%d frames, flow at %.0f%% (%ux%u), %d images\n", pp.deviceName, W, H, flow * 100, G.fw, G.fh, G.nall);
    static const float pans[][2] = { { 0, 0 }, { 4, 0 }, { 12, -8 }, { -30, 14 }, { 64, 24 } };
    for (int count = 1; count <= LS_MAXGEN; count += 2)   // 2x (one picture at 0.5), 4x (three at 0.25, 0.5, 0.75)
    for (unsigned c = 0; c < sizeof pans / sizeof *pans; c++) {
        PX = pans[c][0]; PY = pans[c][1]; G.fresh = 1; frame_no = 0;
        for (int k = 0; k < 3; k++) feed((float)k, 0);
        double ms = feed(3, count);
        fill(before, 2); fill(after, 3);
        for (int k = 0; k < count; k++) {
            float t = (float)(k + 1) / (float)(count + 1); fill(truth, 2 + t);
            for (int i = 0; i < W * H * 4; i++) blend[i] = (uint8_t)(before[i] * (1 - t) + after[i] * t + 0.5f);
            const int M = 48; double e = err(map[1 + k], truth, M), eb = err(blend, truth, M);
            printf("%dx, pan %4.0f,%4.0f px a frame, picture at %.2f: off the true picture by %.2f / 255 (plain mix %.2f)%s", count + 1, PX, PY, t, e, eb, k + 1 < count ? "\n" : "");
            char nm[48]; snprintf(nm, sizeof nm, "lsfg_%dx_pan%u_%d", count + 1, c, k); ppm(dir, nm, map[1 + k]);
            if (PX == 0 && PY == 0) CHECK(e < 1.5, "%dx: a still picture changed: %.2f", count + 1, e);
            else if (fabsf(PX) < 40) CHECK(e < eb, "%dx pan %g,%g at %.2f: %.2f, plain mix %.2f", count + 1, PX, PY, t, e, eb); }
        printf("; the frame and %d picture(s) took %.0f ms\n", count, ms); }
    printf(fails ? "t_lsfg_vk: %d FAILED\n" : "t_lsfg_vk: all passed\n", fails);
    return fails != 0;
}
