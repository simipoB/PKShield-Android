// Frame generation on a real Vulkan driver (any: Mesa's lavapipe on a PC is enough): nvn_fgen.h's own passes and shaders make the picture between two synthetic
// frames, read back and compared with the picture that truly lies between them.
//   make t_fgen_vk VULKAN_INC=<a folder holding vulkan/vulkan.h>     ./t_fgen_vk [folder for .ppm pictures of each case]
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vulkan/vulkan.h>
#include "../nvn/nvn_fgen.h"
#define W 1280
#define H 720
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool pool; static fgen G;
static VkBuffer buf[4]; static VkDeviceMemory bmem[4]; static uint8_t *map[4];   // frame a, frame b, the result, fg_stat.frag's texel
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static float hash(int x, int y, int s) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u; h = (h ^ (h >> 13)) * 1274126177u; return ((h ^ (h >> 16)) & 0xffff) / 65535.0f; }
static float vnoise(float x, float y, int s) { int xi = (int)floorf(x), yi = (int)floorf(y); float fx = x - xi, fy = y - yi; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    return (hash(xi, yi, s) * (1 - fx) + hash(xi + 1, yi, s) * fx) * (1 - fy) + (hash(xi, yi + 1, s) * (1 - fx) + hash(xi + 1, yi + 1, s) * fx) * fy; }
// A scene with detail at several sizes (like a game picture: large shapes, edges, fine texture), defined at any real position so a shifted frame is exact
static void scene(float x, float y, int seed, float *rgb) {
    for (int c = 0; c < 3; c++) { float v = 0.5f * vnoise(x / 97, y / 97, seed + c) + 0.3f * vnoise(x / 23, y / 23, seed + 7 + c) + 0.2f * vnoise(x / 6, y / 6, seed + 13 + c);
        if (vnoise(x / 41, y / 41, seed + 31) > 0.62f) v = 1 - v;   // hard edges
        rgb[c] = v; }
}
typedef void (*paint)(int x, int y, int which, float *rgb);   // which: 0 the frame before, 1 the frame after, 2 the true picture between
static void fill(uint8_t *m, paint f, int which) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float c[3]; f(x, y, which, c);
    for (int k = 0; k < 3; k++) { m[(y * W + x) * 4 + k] = (uint8_t)(fminf(fmaxf(c[k], 0), 1) * 255 + 0.5f); }
    m[(y * W + x) * 4 + 3] = 255; } }
static void run(void) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
    fg_begin(&G, cb);
    VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyBufferToImage(cb, buf[0], G.c[0].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc); vkCmdCopyBufferToImage(cb, buf[1], G.c[1].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    fg_ingest(&G, cb, 0); fg_ingest(&G, cb, 1); fg_generate(&G, cb, 1);
    vkCmdCopyImageToBuffer(cb, G.m.img, VK_IMAGE_LAYOUT_GENERAL, buf[2], 1, &bc);
    bc.imageExtent = (VkExtent3D){ 1, 1, 1 }; vkCmdCopyImageToBuffer(cb, G.st.img, VK_IMAGE_LAYOUT_GENERAL, buf[3], 1, &bc);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(q); vkFreeCommandBuffers(dev, pool, 1, &cb);
}
static void ppm(const char *dir, const char *name, const uint8_t *m) { if (!dir) return; char p[1024]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name); FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6 %d %d 255\n", W, H); for (int i = 0; i < W * H; i++) fwrite(m + i * 4, 1, 3, f); fclose(f); }
// mean absolute error (0..255) of got against want over the rectangle
static double err(const uint8_t *got, const uint8_t *want, int x0, int y0, int x1, int y1, int *worst) {
    double s = 0; int n = 0, mx = 0; for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) for (int k = 0; k < 3; k++) { int d = abs(got[(y * W + x) * 4 + k] - want[(y * W + x) * 4 + k]); s += d; n++; if (d > mx) mx = d; }
    if (worst) { *worst = mx; }
    return s / n;
}
static uint8_t truth[W * H * 4], blend[W * H * 4];
static float half(uint16_t h) { int e = h >> 10 & 31, m = h & 1023; return e ? ldexpf((float)(m | 1024), e - 25) : ldexpf((float)m, -24); }
static float stat(void) { const uint16_t *h = (const uint16_t *)map[3]; return half(h[0]); }   // (fg_stat.frag's texel: how different the frames were)
static void do_case(const char *dir, const char *name, paint f) {
    fill(map[0], f, 0); fill(map[1], f, 1); fill(truth, f, 2);
    for (int i = 0; i < W * H * 4; i++) blend[i] = (uint8_t)((map[0][i] + map[1][i] + 1) / 2);
    run();
    char n[64]; snprintf(n, sizeof n, "%s_a", name); ppm(dir, n, map[0]); snprintf(n, sizeof n, "%s_b", name); ppm(dir, n, map[1]);
    snprintf(n, sizeof n, "%s_mid", name); ppm(dir, n, map[2]); snprintf(n, sizeof n, "%s_truth", name); ppm(dir, n, truth);
}
static float PX, PY;   // the pan of the current case, pixels a frame
static void p_pan(int x, int y, int w, float *c) { float t = w == 0 ? 0 : w == 1 ? 1 : 0.5f; scene(x + PX * t, y + PY * t, 1, c); }
static void p_still(int x, int y, int w, float *c) { (void)w; scene(x, y, 2, c); }
static void p_cut(int x, int y, int w, float *c) { scene(x, y, w == 1 ? 50 : 3, c); }   // (the true in-between of a cut: the frame before, shown again)
static int in_hud(int x, int y) { return (x >= 40 && x < 200 && y >= 30 && y < 70) || (x >= 500 && x < 600 && y >= 300 && y < 330); }
static void p_hud(int x, int y, int w, float *c) {   // a panning scene under two opaque boxes of "text" that do not move
    if (in_hud(x, y)) { float v = ((x / 3 + y / 5) & 1) ? 0.95f : 0.1f; c[0] = c[1] = c[2] = v; return; }
    p_pan(x, y, w, c);
}
static void p_obj(int x, int y, int w, float *c) {   // a still scene, a textured block crossing it at 16 pixels a frame
    float t = w == 0 ? 0 : w == 1 ? 1 : 0.5f, ox = 200 + 16 * t, oy = 120;
    if (x >= ox && x < ox + 120 && y >= oy && y < oy + 90) { scene((x - ox) * 1.7f, (y - oy) * 1.7f, 90, c); c[0] = 0.3f + 0.7f * c[0]; return; }
    scene(x, y, 4, c);
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : NULL;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("SKIP: no Vulkan instance on this machine\n"); return 0; }
    uint32_t n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds); if (!n) { printf("SKIP: no Vulkan device on this machine\n"); return 0; }
    pd = pds[0]; VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp); uint32_t qf = 0; while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qf++;
    float prio = 1; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("SKIP: no Vulkan device could be opened\n"); return 0; }
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf }; vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (int i = 0; i < 4; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT }; vkCreateBuffer(dev, &bci, NULL, &buf[i]);
        VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, buf[i], &rq); uint32_t t = 0;
        while (t < mp.memoryTypeCount && !((rq.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) t++;
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t }; vkAllocateMemory(dev, &mai, NULL, &bmem[i]);
        vkBindBufferMemory(dev, buf[i], bmem[i], 0); void *m; vkMapMemory(dev, bmem[i], 0, VK_WHOLE_SIZE, 0, &m); map[i] = m; }
    if (!fg_setup(&G, dev, pd, VK_FORMAT_R8G8B8A8_UNORM, W, H)) { printf("FAIL: frame generation cannot be set up on %s: %s\n", pp.deviceName, G.why ? G.why : "?"); return 1; }
    printf("frame generation on %s: %dx%d frames, motion from %ux%u down to %ux%u\n", pp.deviceName, W, H, G.lw[0], G.lh[0], G.lw[FG_LEVELS - 1], G.lh[FG_LEVELS - 1]);
    const int M = 48; int worst;   // (the outer band is left out: there the picture comes from one frame alone, off the other's edge)
    static const float pans[][2] = { { 3, 0 }, { 12, -8 }, { -30, 14 }, { 64, 24 }, { -100, -40 } };
    for (unsigned i = 0; i < sizeof pans / sizeof *pans; i++) {
        PX = pans[i][0]; PY = pans[i][1]; char name[32]; snprintf(name, sizeof name, "pan%u", i); do_case(dir, name, p_pan);
        double e = err(map[2], truth, M, M, W - M, H - M, &worst), eb = err(blend, truth, M, M, W - M, H - M, NULL);
        printf("pan %4.0f,%4.0f px a frame: in-between off the true picture by %.2f / 255 (a plain mix of the two frames: %.2f); difference left %.3f\n", PX, PY, e, eb, stat());
        if (fabsf(PX) > 96) CHECK(e <= eb, "a pan past the search range is worse than a plain mix: %.2f, %.2f", e, eb);   // (near the reach of the coarsest level, 4 of its texels = 128 pixels: no promise, but never worse)
        else CHECK(e < 4.0 && (e < 0.35 * eb || e < 1.0), "pan %g,%g: error %.2f, plain mix %.2f", PX, PY, e, eb); }
    do_case(dir, "still", p_still);
    { double e = err(map[2], map[1], 0, 0, W, H, &worst); printf("still picture: off by %.3f, worst %d; difference left %.3f\n", e, worst, stat()); CHECK(worst <= 1, "a still picture changed: worst %d", worst); }
    do_case(dir, "cut", p_cut);
    { double e = err(map[2], map[0], 0, 0, W, H, &worst); printf("cut: off the frame before by %.2f (a plain mix: %.2f); difference left %.3f\n", e, err(blend, map[0], 0, 0, W, H, NULL), stat()); CHECK(e < 1.0, "a cut was mixed: %.2f", e); }
    PX = 12; PY = -8; do_case(dir, "hud", p_hud);
    { double e1 = err(map[2], truth, 40, 30, 200, 70, &worst); int w1 = worst; double e2 = err(map[2], truth, 500, 300, 600, 330, &worst);
      double eo = err(map[2], truth, M, 90, W - M, 280, NULL), eb = err(blend, truth, M, 90, W - M, 280, NULL);
      printf("HUD over a pan: boxes off by %.3f / %.3f (worst %d / %d), the scene between them %.2f (plain mix %.2f)\n", e1, e2, w1, worst, eo, eb);
      CHECK(w1 <= 1 && worst <= 1, "the HUD moved: worst %d / %d", w1, worst); CHECK(eo < 4.0 && eo < 0.35 * eb, "the scene under a HUD: %.2f, plain mix %.2f", eo, eb); }
    do_case(dir, "obj", p_obj);
    { double e = err(map[2], truth, 180, 100, 360, 230, &worst), eb = err(blend, truth, 180, 100, 360, 230, NULL), es = err(map[2], truth, 400, 0, W, H, &worst);
      printf("a block crossing a still scene: around the block off by %.2f (plain mix %.2f); the still part %.3f (worst %d); difference left %.3f\n", e, eb, es, worst, stat());
      CHECK(e < 0.6 * eb, "a moving block: %.2f, plain mix %.2f", e, eb); CHECK(worst <= 1, "the still part changed: worst %d", worst); }
    printf(fails ? "t_fgen_vk: %d FAILED\n" : "t_fgen_vk: all passed\n", fails);
    return fails != 0;
}
