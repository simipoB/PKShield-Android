// Galar Native GPU self-tests: what the engine (dm/gfx/nvn/nvn_vk.c) asks of the phone's own Vulkan driver, recreated with synthetic data.
// No game files, no keys: safe to run on any phone, including shared ones (Samsung Remote Test Lab, Firebase Test Lab, AWS Device Farm).
// Each test mirrors one engine path and prints one line: [PASS] / [WARN] / [FAIL] / [SKIP] / [INFO] name: what it means for the game.
//   float-controls      nvn_vk.c mtl_init: shaders patched by spv_float_controls (SignedZeroInfNanPreserve); NaN / infinity checks with and without it
//   shader-maths        spv_nan_minmax / spv_nan_select: NaN in min / max / saturate, pow as exp2(log2), integers carried in float variables, four ways
//   end-of-submit-barrier  barrier_host: shader writes the CPU reads after the fence, with and without the barrier, coherent and cached memory
//   reinterpreted-format   MUTABLE_FORMAT textures written as one format and sampled as another (compressed render targets: AFBC, DCC)
//   3d-render-target    att_view: a 3D image's slices attached as a layered 2D array (the colour-grading tables; black 3D scenes when only slice 0 drew)
//   dmabuf-heap         the heap route: /dev/dma_heap memory mapped at a fixed address and imported into the driver (the Mali / Xclipse memory pools)
//   bc-textures         the BC layer (gfx/tex/bc_gpu.c, linked as is): BC1-BC7 converted on the GPU only where it matches the CPU bit for bit
// A crash inside the driver (SIGSEGV / SIGBUS / SIGABRT) is caught: that test reports it and the remaining GPU tests are skipped.
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <vulkan/vulkan.h>
#include "bc_gpu.h"
#include "nvn_spv.h"
#include "selftest_spv.h"

void probe_vout(const char *fmt, va_list ap);   // probe.c
static void O(const char *fmt, ...) { va_list ap; va_start(ap, fmt); probe_vout(fmt, ap); va_end(ap); }

enum { PASS, WARN, FAIL, SKIP, INFO };
static int g_count[5];
static void R(int st, const char *name, const char *fmt, ...) {
    static const char *tag[] = { "PASS", "WARN", "FAIL", "SKIP", "INFO" };
    g_count[st]++; O("[%s] %s: ", tag[st], name);
    va_list ap; va_start(ap, fmt); probe_vout(fmt, ap); va_end(ap); O("\n");
}

// ---- crash guard ----
static sigjmp_buf g_jb; static volatile sig_atomic_t g_sig;
static const char *volatile g_step;   // what a test was doing (named in the report when the driver crashes there)
static void on_crash(int s) { g_sig = s; siglongjmp(g_jb, 1); }
static void guard(int on) {
    static struct sigaction old[3]; static const int sigs[3] = { SIGSEGV, SIGBUS, SIGABRT };
    for (int i = 0; i < 3; i++) {
        if (on) { struct sigaction sa = { 0 }; sa.sa_handler = on_crash; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER; sigaction(sigs[i], &sa, &old[i]); }
        else sigaction(sigs[i], &old[i], NULL);
    }
}

// ---- device (as the engine makes it: first GPU, a graphics + compute queue, every core feature on) ----
static VkInstance s_inst; static VkPhysicalDevice s_pd; static VkDevice s_dev; static VkQueue s_q; static uint32_t s_qf, s_api;
static VkCommandPool s_cp; static VkPhysicalDeviceMemoryProperties s_mp; static VkPhysicalDeviceProperties s_props; static VkPhysicalDeviceFeatures s_feat;
static int s_ext_fd, s_ext_dmabuf, s_ext_fctl, s_ext_drvp;
static PFN_vkGetMemoryFdPropertiesKHR p_fdprops;

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < s_mp.memoryTypeCount; i++) { VkMemoryPropertyFlags f = s_mp.memoryTypes[i].propertyFlags; if ((bits >> i & 1) && (f & want) == want && !(f & avoid)) return (int)i; }
    return -1;
}

static int st_device(void) {
    uint32_t iv = VK_API_VERSION_1_0; vkEnumerateInstanceVersion(&iv);
    if (iv < VK_API_VERSION_1_1) { R(SKIP, "device", "the Vulkan loader is %u.%u; Galar Native needs Vulkan 1.1", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv)); return -1; }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Galar Native self-test", .apiVersion = iv < VK_API_VERSION_1_3 ? iv : VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkResult vr = vkCreateInstance(&ici, NULL, &s_inst);
    if (vr) { R(FAIL, "device", "vkCreateInstance %d", vr); return -1; }
    uint32_t n = 1; vkEnumeratePhysicalDevices(s_inst, &n, &s_pd);
    if (!n) { R(FAIL, "device", "no Vulkan GPU"); return -1; }
    vkGetPhysicalDeviceProperties(s_pd, &s_props); vkGetPhysicalDeviceMemoryProperties(s_pd, &s_mp); vkGetPhysicalDeviceFeatures(s_pd, &s_feat);
    s_api = s_props.apiVersion < ai.apiVersion ? s_props.apiVersion : ai.apiVersion;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nq, NULL); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nq, qp);
    for (s_qf = 0; s_qf < nq && (qp[s_qf].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT); s_qf++) { }
    if (s_qf == nq) { R(FAIL, "device", "no queue family with graphics and compute"); return -1; }
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, NULL); VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, ex);
    const char *on[8]; uint32_t non = 0;
    if (has_ext(ex, ne, "VK_KHR_external_memory_fd")) { on[non++] = "VK_KHR_external_memory_fd"; s_ext_fd = 1;
        if (has_ext(ex, ne, "VK_EXT_external_memory_dma_buf")) { on[non++] = "VK_EXT_external_memory_dma_buf"; s_ext_dmabuf = 1; } }
    if (s_api < VK_API_VERSION_1_2) {   // Vulkan 1.1: float controls and the driver's identity are extensions (nvn_vk.c, older Mali-G52 / G72 / G76)
        if (has_ext(ex, ne, "VK_KHR_shader_float_controls")) { on[non++] = "VK_KHR_shader_float_controls"; s_ext_fctl = 1; }
        if (has_ext(ex, ne, "VK_KHR_driver_properties")) { on[non++] = "VK_KHR_driver_properties"; s_ext_drvp = 1; }
    }
    free(ex);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = s_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = non, .ppEnabledExtensionNames = on, .pEnabledFeatures = &s_feat };
    if ((vr = vkCreateDevice(s_pd, &dci, NULL, &s_dev))) { R(FAIL, "device", "vkCreateDevice %d with every core feature on (as the engine asks)", vr); return -1; }
    vkGetDeviceQueue(s_dev, s_qf, 0, &s_q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s_qf };
    vkCreateCommandPool(s_dev, &pci, NULL, &s_cp);
    if (s_ext_fd) p_fdprops = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(s_dev, "vkGetMemoryFdPropertiesKHR");
    return 0;
}

// ---- helpers ----
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; int coherent, type; } hbuf;
static int hbuf_make(hbuf *h, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    memset(h, 0, sizeof *h); h->type = -1;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    if (vkCreateBuffer(s_dev, &bi, NULL, &h->b)) { h->b = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(s_dev, h->b, &rq);
    int t = mem_type(rq.memoryTypeBits, want, avoid);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || vkAllocateMemory(s_dev, &ai, NULL, &h->m)) { vkDestroyBuffer(s_dev, h->b, NULL); memset(h, 0, sizeof *h); h->type = -1; return -1; }
    vkBindBufferMemory(s_dev, h->b, h->m, 0);
    if (vkMapMemory(s_dev, h->m, 0, VK_WHOLE_SIZE, 0, &h->p)) h->p = NULL;
    h->coherent = !!(s_mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); h->type = t;
    return t;
}
static void hbuf_free(hbuf *h) { if (h->m) { if (h->p) vkUnmapMemory(s_dev, h->m); vkFreeMemory(s_dev, h->m, NULL); } if (h->b) vkDestroyBuffer(s_dev, h->b, NULL); memset(h, 0, sizeof *h); }
static void hbuf_sync(hbuf *h, int to_gpu) {
    if (h->coherent || !h->m) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = h->m, .size = VK_WHOLE_SIZE };
    if (to_gpu) vkFlushMappedMemoryRanges(s_dev, 1, &r); else vkInvalidateMappedMemoryRanges(s_dev, 1, &r);
}
static VkCommandBuffer cb_begin(void) {
    VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s_cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(s_dev, &ca, &cb)) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi); return cb;
}
static VkResult cb_submit(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f; VkResult r = vkCreateFence(s_dev, &fi, NULL, &f);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (!r) { r = vkQueueSubmit(s_q, 1, &si, f); if (!r) r = vkWaitForFences(s_dev, 1, &f, VK_TRUE, 5000000000ull); vkDestroyFence(s_dev, f, NULL); }
    vkFreeCommandBuffers(s_dev, s_cp, 1, &cb); return r;
}
// the engine's end-of-submission barrier (nvn_vk.c barrier_host), as is
static void barrier_host(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void barrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 1, &mb, 0, NULL, 0, NULL);
}
typedef struct { VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; VkDescriptorPool dp; VkDescriptorSet ds; } comp;
static void comp_free(comp *c) {
    if (c->pipe) vkDestroyPipeline(s_dev, c->pipe, NULL); if (c->pl) vkDestroyPipelineLayout(s_dev, c->pl, NULL); if (c->dp) vkDestroyDescriptorPool(s_dev, c->dp, NULL);
    if (c->dsl) vkDestroyDescriptorSetLayout(s_dev, c->dsl, NULL); if (c->sm) vkDestroyShaderModule(s_dev, c->sm, NULL); memset(c, 0, sizeof *c);
}
static VkResult comp_make(comp *c, const uint32_t *code, size_t words, const VkDescriptorType *types, uint32_t nb, uint32_t push_bytes) {
    memset(c, 0, sizeof *c); VkResult r;
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code };
    if ((r = vkCreateShaderModule(s_dev, &mci, NULL, &c->sm))) return r;
    VkDescriptorSetLayoutBinding b[4]; VkDescriptorPoolSize ps[4];
    for (uint32_t i = 0; i < nb; i++) { b[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorType = types[i], .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }; ps[i] = (VkDescriptorPoolSize){ types[i], 1 }; }
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b };
    if ((r = vkCreateDescriptorSetLayout(s_dev, &dli, NULL, &c->dsl))) return r;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c->dsl, .pushConstantRangeCount = push_bytes ? 1 : 0, .pPushConstantRanges = &pr };
    if ((r = vkCreatePipelineLayout(s_dev, &pli, NULL, &c->pl))) return r;
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = c->pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = c->sm, .pName = "main" } };
    if ((r = vkCreateComputePipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &c->pipe))) { c->pipe = VK_NULL_HANDLE; return r; }
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = nb, .pPoolSizes = ps };
    if ((r = vkCreateDescriptorPool(s_dev, &dpi, NULL, &c->dp))) return r;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp, .descriptorSetCount = 1, .pSetLayouts = &c->dsl };
    return vkAllocateDescriptorSets(s_dev, &dai, &c->ds);
}
static void ds_buffer(comp *c, uint32_t binding, VkBuffer b) {
    VkDescriptorBufferInfo bi = { b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
    vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
}
static VkDeviceMemory image_mem(VkImage img) {
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(s_dev, img, &rq);
    int t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); if (t < 0) t = mem_type(rq.memoryTypeBits, 0, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)t };
    VkDeviceMemory m = VK_NULL_HANDLE; if (t < 0 || vkAllocateMemory(s_dev, &ai, NULL, &m)) return VK_NULL_HANDLE;
    vkBindImageMemory(s_dev, img, m, 0); return m;
}

// ---- float controls ----
static const char *FC_NAME[8] = { "isnan(NaN)", "isinf(inf)", "sign of -0", "inf*0 is NaN", "1/0 is inf", "NaN != NaN", "inf-inf is NaN", "inf+1 is inf" };
static const uint32_t FC_WANT[8] = { 1, 1, 0x80000000u, 1, 1, 1, 1, 0x7f800000u };
// 0 ran (out filled), else the VkResult that stopped it
static VkResult run_floatctl(const uint32_t *code, size_t words, uint32_t out[8]) {
    hbuf h; if (hbuf_make(&h, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0) < 0 &&
                  hbuf_make(&h, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t *p = h.p; static const uint32_t in[8] = { 0x7f800000u, 0x7fc00000u, 0x80000000u, 0, 0x3f800000u, 0, 0, 0 };
    memcpy(p, in, sizeof in); memset(p + 8, 0xee, 32); hbuf_sync(&h, 1);
    comp c; VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; VkResult r = comp_make(&c, code, words, &t, 1, 0);
    if (!r) {
        ds_buffer(&c, 0, h.b); VkCommandBuffer cb = cb_begin();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1); barrier_host(cb);
        if (!(r = cb_submit(cb))) { hbuf_sync(&h, 0); memcpy(out, p + 8, 32); }
    }
    comp_free(&c); hbuf_free(&h); return r;
}
static int fc_wrong(const uint32_t out[8], char *list, size_t n) {
    int bad = 0; list[0] = 0;
    for (int i = 0; i < 8; i++) if (out[i] != FC_WANT[i]) { size_t l = strlen(list); snprintf(list + l, n - l, "%s%s (got %#x)", bad++ ? ", " : "", FC_NAME[i], out[i]); }
    return bad;
}
static void t_floatctl(void) {
    const char *N = "float-controls";
    int drvp = s_api >= VK_API_VERSION_1_2 || s_ext_drvp, fcp = s_api >= VK_API_VERSION_1_2 || s_ext_fctl;
    VkPhysicalDeviceDriverProperties dp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceFloatControlsProperties fc = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, .pNext = drvp ? &dp : NULL };
    VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = fcp ? (void *)&fc : drvp ? (void *)&dp : NULL };
    vkGetPhysicalDeviceProperties2(s_pd, &pp);
    int arm = drvp ? dp.driverID == VK_DRIVER_ID_ARM_PROPRIETARY : s_props.vendorID == 0x13B5;
    arm |= drvp ? dp.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY : s_props.vendorID == 0x144D;   // (the engine asks on Samsung's Xclipse driver too, since stock-driver-default)
    int support = fcp && fc.shaderSignedZeroInfNanPreserveFloat32;
    O("  driver %s (id %d)%s; SignedZeroInfNanPreserve float32: %s; the engine asks for it by default: %s\n", drvp ? dp.driverName : "(no driver properties)", (int)dp.driverID,
      arm ? ", ARM's own" : "", support ? "supported" : "not supported", arm && support ? "yes" : "no");
    size_t words = sizeof spv_floatctl / 4; uint32_t plain[8], patched[8]; char wrong[512], wrong2[512];
    VkResult r = run_floatctl(spv_floatctl, words, plain);
    if (r) { R(FAIL, N, "the plain test shader did not run (%d)", r); return; }
    int bad = fc_wrong(plain, wrong, sizeof wrong);
    O("  without float controls: %s\n", bad ? wrong : "all 8 checks right");
    if (!support) {
        if (!bad) R(PASS, N, "keeps infinities and NaN without being asked (the driver cannot be asked)");
        else R(WARN, N, "wrong without float controls (%s) and the driver cannot be asked to keep them: shaders that guard with isnan may draw white or black", wrong);
        return;
    }
    size_t nw = 0; uint32_t *code = spv_float_controls(spv_floatctl, words, &nw);
    if (!code) { R(FAIL, N, "spv_float_controls did not patch the test shader (probe bug)"); return; }
    r = run_floatctl(code, nw, patched); free(code);
    if (r) { R(FAIL, N, "the driver refused a shader asking for float controls (%d): the engine's own start-up test turns them off, and %s", r,
               bad ? "shaders can then draw wrong" : "this driver is right without them anyway"); return; }
    int bad2 = fc_wrong(patched, wrong2, sizeof wrong2);
    if (bad2) R(FAIL, N, "wrong even with float controls: %s", wrong2);
    else if (!bad) R(PASS, N, "right with and without float controls");
    else if (arm) R(PASS, N, "needs float controls (without: %s); the engine asks for them on this driver", wrong);
    else R(WARN, N, "needs float controls (without: %s), but the engine only asks on ARM's and Samsung's drivers: this driver's profile should turn them on (DM_VK_FLOAT_CONTROLS=1)", wrong);
}

// ---- shader maths: NaN in min / max / saturate, pow as exp2(log2), integers carried in floats ----
enum { SM_N = 16 };
static const char *SM_NAME[SM_N] = { "min(NaN, 1)", "min(1, NaN)", "max(NaN, 0.5)", "max(0.5, NaN)", "saturate(NaN)", "clamp(NaN, lo, hi)", "max(vec2(NaN, ..), 0).x", "pow(0, 2.2)",
    "log2(0)", "exp2(-inf)", "pow(0, 0) is NaN", "saturate(pow(0, 0))", "255 carried in a float", "carried 255 / 255", "fma(0.5, 1, 0.5)", "mix(0.5, 1, saturate(NaN))" };
static const uint32_t SM_WANT[SM_N] = { 0x3f800000u, 0x3f800000u, 0x3f000000u, 0x3f000000u, 0, 0, 0, 0, 0xff800000u, 0, 1, 0, 255, 0x3f800000u, 0x3f800000u, 0x3f000000u };
static VkResult run_shmath(const uint32_t *code, size_t words, uint32_t out[SM_N]) {
    hbuf h; if (hbuf_make(&h, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0) < 0 &&
                  hbuf_make(&h, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t *p = h.p; static const uint32_t in[16] = { 0x7f800000u, 0x7fc00000u, 0x80000000u, 0, 0x3f800000u, 0x3f000000u, 0x400ccccdu, 0,   255, 128, 0, 0, 0, 0, 0, 0 };
    memcpy(p, in, sizeof in); memset(p + 16, 0xee, 4 * SM_N); hbuf_sync(&h, 1);
    comp c; VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; VkResult r = comp_make(&c, code, words, &t, 1, 0);
    if (!r) {
        ds_buffer(&c, 0, h.b); VkCommandBuffer cb = cb_begin();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1); barrier_host(cb);
        if (!(r = cb_submit(cb))) { hbuf_sync(&h, 0); memcpy(out, p + 16, 4 * SM_N); }
    }
    comp_free(&c); hbuf_free(&h); return r;
}
static int sm_wrong(const uint32_t out[SM_N], char *list, size_t n) {
    int bad = 0; list[0] = 0;
    for (int i = 0; i < SM_N; i++) if (out[i] != SM_WANT[i]) { size_t l = strlen(list); snprintf(list + l, n - l, "%s%s (got %#x)", bad++ ? ", " : "", SM_NAME[i], out[i]); }
    return bad;
}
// The test shader four ways: as compiled; with float controls; with float controls + spv_nan_minmax (NMin / NMax / NClamp: what v0.57-v0.59 gave Arm's driver);
// and with spv_nan_select on top (the rule written out with isnan: what the engine gives Arm's driver now). The last one is the verdict
static void t_shmath(void) {
    const char *N = "shader-maths";
    static const char *WAY[4] = { "as compiled", "float controls", "float controls + NMin / NMax / NClamp", "float controls + NMin / NMax / NClamp + isnan written out" };
    int fcp = (s_api >= VK_API_VERSION_1_2 || s_ext_fctl), bad[4] = { -1, -1, -1, -1 }; char wrong[4][768]; VkResult rr[4] = { 0 };
    if (fcp) { VkPhysicalDeviceFloatControlsProperties fc = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES };
        VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &fc }; vkGetPhysicalDeviceProperties2(s_pd, &pp); fcp = fc.shaderSignedZeroInfNanPreserveFloat32; }
    for (int way = 0; way < 4; way++) {
        if (way && !fcp) break;
        size_t words = sizeof spv_shmath / 4, nw; uint32_t *code = malloc(4 * words), *c2; if (!code) return; memcpy(code, spv_shmath, 4 * words);
        if (way >= 1 && (c2 = spv_float_controls(code, words, &nw))) { free(code); code = c2; words = nw; }
        if (way >= 2 && (c2 = spv_nan_minmax(code, words, &nw))) { free(code); code = c2; words = nw; }
        if (way >= 3) { if ((c2 = spv_nan_select(code, words, &nw))) { free(code); code = c2; words = nw; } else { free(code); R(FAIL, N, "spv_nan_select did not patch the test shader (probe bug)"); return; } }
        uint32_t out[SM_N]; g_step = WAY[way]; rr[way] = run_shmath(code, words, out); g_step = NULL; free(code);
        if (rr[way]) { O("  %s: the shader did not run (%d)\n", WAY[way], rr[way]); continue; }
        bad[way] = sm_wrong(out, wrong[way], sizeof wrong[way]);
        O("  %s: %s\n", WAY[way], bad[way] ? wrong[way] : "all 16 answers as on the Switch's GPU");
    }
    int last = fcp ? 3 : 0;
    if (bad[last] < 0) { R(FAIL, N, "the test shader did not run %s (%d)", WAY[last], rr[last]); return; }
    if (!fcp) { if (!bad[0]) R(PASS, N, "right as compiled (the driver cannot be asked for float controls)"); else R(WARN, N, "wrong and the driver cannot be asked for float controls: %s", wrong[0]); return; }
    if (bad[3]) R(FAIL, N, "wrong even with the rule written out: %s: lit scenes can still come out white or black here", wrong[3]);
    else if (bad[2] > 0) R(PASS, N, "right only with the NaN rule written out; this driver does not honour NMin / NMax / NClamp (%s): the white screen of v0.57-v0.59", wrong[2]);
    else if (bad[2] == 0 && bad[1] > 0) R(PASS, N, "right with NMin / NMax / NClamp and with the rule written out (plain min / max / clamp: %s)", wrong[1]);
    else R(PASS, N, "right with float controls alone");
}

// ---- end-of-submission barrier ----
static void t_barrier(void) {
    const char *N = "end-of-submit-barrier"; const uint32_t n = 1u << 20; const int rounds = 12;
    comp c; VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkResult r = comp_make(&c, spv_barrier, sizeof spv_barrier / 4, &t, 1, 8);
    if (r) { R(FAIL, N, "the test shader did not build (%d)", r); comp_free(&c); return; }
    static const struct { const char *name; VkMemoryPropertyFlags want, avoid; } K[] = {
        { "coherent", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 },
        { "cached, not coherent", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT },
        { "cached, coherent", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 },
    };
    int seen[VK_MAX_MEMORY_TYPES] = { 0 }, stale_with = 0, stale_without = 0, kinds = 0; char notes[512] = "";
    for (size_t k = 0; k < sizeof K / sizeof *K; k++) {
        hbuf h; if (hbuf_make(&h, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, K[k].want, K[k].avoid) < 0 || !h.p) { hbuf_free(&h); continue; }
        if (seen[h.type]) { hbuf_free(&h); continue; } seen[h.type] = 1; kinds++;
        ds_buffer(&c, 0, h.b);
        int stale[2] = { 0, 0 };
        for (int with = 0; with < 2; with++) for (int round = 0; round < rounds; round++) {
            uint32_t seed = 0x51ed0000u + (uint32_t)(with * 100 + round), *p = h.p;
            memset(p, 0xde, (size_t)n * 4); hbuf_sync(&h, 1);
            VkCommandBuffer cb = cb_begin(); uint32_t pc[2] = { seed, n };
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
            vkCmdPushConstants(cb, c.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc); vkCmdDispatch(cb, n / 64, 1, 1);
            if (with) barrier_host(cb);
            if ((r = cb_submit(cb))) { R(FAIL, N, "submission failed (%d) on %s memory", r, K[k].name); hbuf_free(&h); comp_free(&c); return; }
            hbuf_sync(&h, 0);
            uint32_t badw = 0; for (uint32_t i = 0; i < n; i++) if (p[i] != (seed ^ (i * 2654435761u))) badw++;
            if (badw) stale[with]++;
        }
        size_t l = strlen(notes);
        snprintf(notes + l, sizeof notes - l, "%s%s (type %d): stale reads in %d/%d rounds without the barrier, %d/%d with it", l ? "; " : "", K[k].name, h.type, stale[0], rounds, stale[1], rounds);
        stale_without += stale[0]; stale_with += stale[1]; hbuf_free(&h);
    }
    comp_free(&c);
    if (!kinds) { R(SKIP, N, "no host-visible memory type for a storage buffer"); return; }
    O("  %s\n", notes);
    if (stale_with) R(FAIL, N, "the CPU read stale GPU writes even after the engine's barrier: the game can read old data back (white / frozen pictures)");
    else if (stale_without) R(PASS, N, "this driver needs the end-of-submission barrier (stale without it); the engine has it since v0.53");
    else R(PASS, N, "GPU writes reach the CPU with and without the barrier");
}

// ---- reinterpreted format: written as RGBA8, sampled as R32_UINT ----
static VkRenderPass make_pass(VkFormat f, VkImageLayout final, VkPipelineStageFlags ds, VkAccessFlags da) {
    VkAttachmentDescription a = { .format = f, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, ds, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, da, 0 };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp = VK_NULL_HANDLE; vkCreateRenderPass(s_dev, &ci, NULL, &rp); return rp;
}
static void t_reinterp(void) {
    const char *N = "reinterpreted-format"; const uint32_t W = 16;
    VkFormatProperties a, b; vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_R8G8B8A8_UNORM, &a); vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_R32_UINT, &b);
    if (!(a.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !(b.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { R(SKIP, N, "RGBA8 targets or R32_UINT sampling missing"); return; }
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, W, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory im = VK_NULL_HANDLE; VkImageView v8 = VK_NULL_HANDLE, v32 = VK_NULL_HANDLE; VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE; VkSampler smp = VK_NULL_HANDLE; comp c = { 0 }; hbuf h = { 0 }; VkResult r;
    if (vkCreateImage(s_dev, &ii, NULL, &img) || !(im = image_mem(img))) { R(FAIL, N, "a MUTABLE_FORMAT RGBA8 target could not be made"); goto out; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCreateImageView(s_dev, &vi, NULL, &v8); vi.format = VK_FORMAT_R32_UINT;
    if ((r = vkCreateImageView(s_dev, &vi, NULL, &v32))) { R(FAIL, N, "an R32_UINT view of an RGBA8 texture was refused (%d)", r); goto out; }
    rp = make_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &v8, .width = W, .height = W, .layers = 1 };
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    if (!rp || vkCreateFramebuffer(s_dev, &fi, NULL, &fb) || vkCreateSampler(s_dev, &si, NULL, &smp)) { R(FAIL, N, "render pass / framebuffer / sampler setup failed"); goto out; }
    if (hbuf_make(&h, W * W * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "no host-visible storage buffer"); goto out; }
    VkDescriptorType ts[2] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    if ((r = comp_make(&c, spv_reinterp, sizeof spv_reinterp / 4, ts, 2, 0))) { R(FAIL, N, "the test shader did not build (%d)", r); goto out; }
    VkDescriptorImageInfo dii = { smp, v32, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c.ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL); ds_buffer(&c, 1, h.b);
    memset(h.p, 0xee, W * W * 4); hbuf_sync(&h, 1);
    VkCommandBuffer cb = cb_begin();
    VkClearValue cv = { .color = { .float32 = { 1, 0, 1, 0 } } };   // R FF G 00 B FF A 00 → 0x00FF00FF (the render pass's clear: a driver's fast-clear path)
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, W } }, .clearValueCount = 1, .pClearValues = &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = { .float32 = { 0, 1, 0, 1 } } } };   // left half 0xFF00FF00 (a drawn clear)
    VkClearRect cr = { { { 0, 0 }, { W / 2, W } }, 0, 1 }; vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1); barrier_host(cb);
    if ((r = cb_submit(cb))) { R(FAIL, N, "submission failed (%d)", r); goto out; }
    hbuf_sync(&h, 0);
    { const uint32_t *p = h.p; int bad_l = 0, bad_r = 0; uint32_t first = 0; int have = 0;
      for (uint32_t i = 0; i < W * W; i++) { uint32_t want = (i % W) < W / 2 ? 0xFF00FF00u : 0x00FF00FFu; if (p[i] != want) { if (!have) { first = p[i]; have = 1; } if ((i % W) < W / 2) bad_l++; else bad_r++; } }
      if (!bad_l && !bad_r) R(PASS, N, "a render target sampled as another format reads the bytes that were drawn (fast clear and drawn clear)");
      else R(FAIL, N, "sampled as R32_UINT, %d texels of the drawn clear and %d of the fast clear are wrong (first %#x): textures the game re-reads as another type come out wrong (compressed render target not resolved)", bad_l, bad_r, first); }
out:
    comp_free(&c); hbuf_free(&h);
    if (smp) vkDestroySampler(s_dev, smp, NULL); if (fb) vkDestroyFramebuffer(s_dev, fb, NULL); if (rp) vkDestroyRenderPass(s_dev, rp, NULL);
    if (v32) vkDestroyImageView(s_dev, v32, NULL); if (v8) vkDestroyImageView(s_dev, v8, NULL); if (img) vkDestroyImage(s_dev, img, NULL); if (im) vkFreeMemory(s_dev, im, NULL);
}

// ---- 3D render target: the game's 32x32x32 colour-grading tables are drawn into a 3D image through 2D-array views of its slices ----
// layered (the engine's att_view): one view of all 32 slices, a 32-layer framebuffer, one pass; slices: a 1-slice view, framebuffer and pass per slice (a candidate
//   workaround where the layered way fails). Each step is named before it runs, so a driver crash (Xclipse 540) says where. Pass 1 clears every slice in the render
//   pass (submitted on its own), pass 2 loads and clears slices 16-31 with vkCmdClearAttachments; then all 32 are read back
static int check_3d(const uint32_t *p, uint32_t D, char *why, size_t n) {
    int bad = 0, first = -1; uint32_t got = 0;
    for (uint32_t z = 0; z < D; z++) { uint32_t want = z < D / 2 ? 0xFF0000FFu : 0xFFFF0000u; int sb = 0;
        for (uint32_t i = 0; i < D * D; i++) if (p[z * D * D + i] != want) { if (!sb) got = p[z * D * D + i]; sb = 1; }
        if (sb) { bad++; if (first < 0) first = (int)z; } }
    if (bad) snprintf(why, n, "%d of 32 slices wrong (first slice %d holds %#x)", bad, first, got);
    return bad;
}
static void t_3d_mode(int layered) {
    const char *N = layered ? "3d-layered" : "3d-slices"; const uint32_t D = 32;
    g_step = "image format query";
    VkImageFormatProperties ifp; VkResult r = vkGetPhysicalDeviceImageFormatProperties(s_pd, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, &ifp);
    if (r) { R(FAIL, N, "3D colour targets drawn through 2D-array views are not supported (%d): the game's 3D colour-grading tables cannot be filled", r); return; }
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, .imageType = VK_IMAGE_TYPE_3D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { D, D, D }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory im = VK_NULL_HANDLE; VkImageView v[32] = { 0 }; VkFramebuffer fb[32] = { 0 }; VkRenderPass rp[2] = { 0 }; hbuf h = { 0 };
    uint32_t nv = layered ? 1 : D; char why[160] = "";
    g_step = "3D image creation";
    if (vkCreateImage(s_dev, &ii, NULL, &img) || !(im = image_mem(img))) { R(FAIL, N, "a 32x32x32 3D target could not be made"); goto out; }
    for (uint32_t i = 0; i < nv; i++) {
        g_step = layered ? "the 32-slice 2D-array view" : "a 1-slice 2D-array view";
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layered ? 0 : i, layered ? D : 1 } };
        if ((r = vkCreateImageView(s_dev, &vi, NULL, &v[i]))) { R(FAIL, N, "%s was refused (%d)", g_step, r); goto out; }
    }
    // pass 0: clear everything (load op CLEAR), pass 1: keep (LOAD) and clear part with vkCmdClearAttachments; both GENERAL as the engine's passes
    for (int k = 0; k < 2; k++) {
        g_step = "render pass creation";
        VkAttachmentDescription a = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = k ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = k ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
        VkSubpassDependency dep[2] = {
            { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
            { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
        if ((r = vkCreateRenderPass(s_dev, &rci, NULL, &rp[k]))) { R(FAIL, N, "render pass refused (%d)", r); goto out; }
    }
    for (uint32_t i = 0; i < nv; i++) {
        g_step = layered ? "the 32-layer framebuffer" : "a 1-layer framebuffer";
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp[0], .attachmentCount = 1, .pAttachments = &v[i], .width = D, .height = D, .layers = layered ? D : 1 };
        if ((r = vkCreateFramebuffer(s_dev, &fi, NULL, &fb[i]))) { R(FAIL, N, "%s was refused (%d)", g_step, r); goto out; }
    }
    if (hbuf_make(&h, D * D * D * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "no host-visible readback buffer"); goto out; }
    memset(h.p, 0, D * D * D * 4); hbuf_sync(&h, 1);
    for (int k = 0; k < 2; k++) {   // two submissions, so a crash names the clear that caused it
        g_step = k ? "recording the per-layer vkCmdClearAttachments" : "recording the render-pass clear of every slice";
        VkCommandBuffer cb = cb_begin();
        VkClearValue cv = { .color = { .float32 = { 1, 0, 0, 1 } } };   // every slice 0xFF0000FF
        for (uint32_t i = 0; i < nv; i++) {
            if (k && !layered && i < D / 2) continue;   // (slices: only 16-31 get the second pass)
            VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp[k], .framebuffer = fb[i], .renderArea = { { 0, 0 }, { D, D } },
                                          .clearValueCount = k ? 0 : 1, .pClearValues = k ? NULL : &cv };
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            if (k) { VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = { .float32 = { 0, 0, 1, 1 } } } };   // slices 16-31 0xFFFF0000
                     VkClearRect cr = { { { 0, 0 }, { D, D } }, layered ? D / 2 : 0, layered ? D / 2 : 1 }; vkCmdClearAttachments(cb, 1, &ca, 1, &cr); }
            vkCmdEndRenderPass(cb);
        }
        if (k) { VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { D, D, D } };
                 vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, h.b, 1, &bc);
                 barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT); }
        g_step = k ? "running the per-layer clear and the read-back" : "running the render-pass clear of every slice";
        if ((r = cb_submit(cb))) { R(FAIL, N, "%s failed (%d)", g_step, r); goto out; }
    }
    g_step = "checking the slices"; hbuf_sync(&h, 0);
    if (!check_3d(h.p, D, why, sizeof why))
        R(PASS, N, layered ? "all 32 slices of a 3D target drew as a layered 2D array (the engine's way): render-pass clear and per-layer clear"
                           : "all 32 slices drew one pass per slice (1-slice views): a way to fill 3D colour-grading tables where the layered way fails");
    else R(FAIL, N, "%s: 3D scenes that use colour-grading tables can come out black or wrong this way", why);
out:
    g_step = NULL; hbuf_free(&h);
    for (uint32_t i = 0; i < 32; i++) { if (fb[i]) vkDestroyFramebuffer(s_dev, fb[i], NULL); if (v[i]) vkDestroyImageView(s_dev, v[i], NULL); }
    for (int k = 0; k < 2; k++) if (rp[k]) vkDestroyRenderPass(s_dev, rp[k], NULL);
    if (img) vkDestroyImage(s_dev, img, NULL); if (im) vkFreeMemory(s_dev, im, NULL);
}
static void t_3d_slices(void) { t_3d_mode(0); }
static void t_3d_layered(void) { t_3d_mode(1); }

// ---- the engine's stock-driver fallback (nvn_vk.c rend_begin / pso_make with sst): a classic render pass in GENERAL layout, draw state built into the pipelines ----
typedef struct { VkCullModeFlags cull; VkFrontFace front; VkPrimitiveTopology topo; VkBool32 dt, dw, st; VkCompareOp scmp; VkStencilOp spass; } fbs;
static VkPipeline fb_pipe(VkRenderPass rp, VkPipelineLayout pl, VkShaderModule vm, VkShaderModule fm, const fbs *s) {
    VkPipelineShaderStageCreateInfo st[2] = { { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
                                              { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };   // the stride in the pipeline (no VERTEX_INPUT_BINDING_STRIDE)
    VkVertexInputAttributeDescription va[2] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32_SFLOAT, 8 } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                                .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = s->topo };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = s->cull,
                                                  .frontFace = s->front, .depthBiasEnable = VK_TRUE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState so = { VK_STENCIL_OP_KEEP, s->spass, VK_STENCIL_OP_KEEP, s->scmp, 0, 0, 0 };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = s->dt, .depthWriteEnable = s->dw,
                                                 .depthCompareOp = s->dt ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS, .stencilTestEnable = s->st, .front = so, .back = so };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo bs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[7] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,   // the engine's 7 (Vulkan 1.0)
                              VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 7, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &bs, .pDynamicState = &dy, .layout = pl, .renderPass = rp };
    VkPipeline p = VK_NULL_HANDLE; return vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gi, NULL, &p) ? VK_NULL_HANDLE : p;
}
static void quad(float *v, float x0, float y0, float x1, float y1, float z) {   // two triangles of the same winding, 4 floats (x, y, z, pad) per vertex
    const float p[6][2] = { { x0, y0 }, { x0, y1 }, { x1, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
    for (int i = 0; i < 6; i++) { v[i * 4] = p[i][0]; v[i * 4 + 1] = p[i][1]; v[i * 4 + 2] = z; v[i * 4 + 3] = 0; }
}
static void t_fallback(void) {
    const char *N = "fallback-draw"; const uint32_t W = 32;
    VkFormat dsf = VK_FORMAT_UNDEFINED; static const VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (int i = 0; i < 2 && !dsf; i++) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(s_pd, cand[i], &fp); if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) dsf = cand[i]; }
    if (!dsf) { R(SKIP, N, "no depth / stencil target format"); return; }
    VkImage ci = VK_NULL_HANDLE, di = VK_NULL_HANDLE; VkDeviceMemory cm = VK_NULL_HANDLE, dm = VK_NULL_HANDLE; VkImageView cv = VK_NULL_HANDLE, dv = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE; VkFramebuffer fb = VK_NULL_HANDLE; VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pp[6] = { 0 }; hbuf vbuf = { 0 }, rb = { 0 }; VkResult r;
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, W, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    if (vkCreateImage(s_dev, &ii, NULL, &ci) || !(cm = image_mem(ci))) { R(FAIL, N, "colour target"); goto out; }
    ii.format = dsf; ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vkCreateImage(s_dev, &ii, NULL, &di) || !(dm = image_mem(di))) { R(FAIL, N, "depth / stencil target"); goto out; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ci, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCreateImageView(s_dev, &vi, NULL, &cv);
    vi.image = di; vi.format = dsf; vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; vkCreateImageView(s_dev, &vi, NULL, &dv);
    {   // the engine's render pass (rp_get): GENERAL layouts, cleared, stored, ordered like barrier_all on both sides
        VkAttachmentDescription a[2] = {
            { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL },
            { 0, dsf, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL } };
        VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_GENERAL }, dr = { 1, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cr, .pDepthStencilAttachment = &dr };
        VkSubpassDependency dep[2] = {
            { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
            { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
        if ((r = vkCreateRenderPass(s_dev, &rci, NULL, &rp))) { R(FAIL, N, "the render pass was refused (%d)", r); goto out; }
        VkImageView at[2] = { cv, dv };
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = at, .width = W, .height = W, .layers = 1 };
        if ((r = vkCreateFramebuffer(s_dev, &fi, NULL, &fb))) { R(FAIL, N, "the framebuffer was refused (%d)", r); goto out; }
    }
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof spv_fallback_vert, .pCode = spv_fallback_vert };
    vkCreateShaderModule(s_dev, &mci, NULL, &vm); mci.codeSize = sizeof spv_fallback_frag; mci.pCode = spv_fallback_frag; vkCreateShaderModule(s_dev, &mci, NULL, &fm);
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (!vm || !fm || vkCreatePipelineLayout(s_dev, &pli, NULL, &pl)) { R(FAIL, N, "shaders / pipeline layout"); goto out; }
    // A strip over everything (red, writes depth 0.5 and stencil 1); top quarters: the same triangles culled BACK and culled FRONT (green: exactly one draws);
    // bottom left: stencil EQUAL 1 in front (blue, draws); bottom right: behind with depth LESS (white, must not draw), then depth test off but stencil EQUAL 0 (yellow, must not draw)
    const fbs S[6] = {
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, 1, 1, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE },
        { VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 0, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 0, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 1, VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 0, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, 1, VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP } };
    for (int i = 0; i < 6; i++) if (!(pp[i] = fb_pipe(rp, pl, vm, fm, &S[i]))) { R(FAIL, N, "pipeline %d with its draw state built in was refused", i); goto out; }
    float vd[34 * 4]; const float strip[4][2] = { { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) { vd[i * 4] = strip[i][0]; vd[i * 4 + 1] = strip[i][1]; vd[i * 4 + 2] = 0.5f; vd[i * 4 + 3] = 0; }
    quad(vd + 4 * 4, -1, -1, 0, 0, 0.3f); quad(vd + 10 * 4, 0, -1, 1, 0, 0.3f); quad(vd + 16 * 4, -1, 0, 0, 1, 0.3f); quad(vd + 22 * 4, 0, 0, 1, 1, 0.9f); quad(vd + 28 * 4, 0, 0, 1, 1, 0.1f);
    if (hbuf_make(&vbuf, sizeof vd, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0 ||
        hbuf_make(&rb, W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "buffers"); goto out; }
    memcpy(vbuf.p, vd, sizeof vd); hbuf_sync(&vbuf, 1);
    VkCommandBuffer cb = cb_begin();
    VkClearValue clr[2]; memset(clr, 0, sizeof clr); clr[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, W } }, .clearValueCount = 2, .pClearValues = clr };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v = { 0, 0, (float)W, (float)W, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, W } }; float bc[4] = { 0, 0, 0, 0 };
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc); vkCmdSetDepthBias(cb, 0, 0, 0); vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    VkDeviceSize zero = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vbuf.b, &zero);   // (Vulkan 1.0's bind: no strides)
    static const float col[6][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 0, 1 } };
    static const uint32_t first[6] = { 0, 4, 10, 16, 22, 28 }, count[6] = { 4, 6, 6, 6, 6, 6 }, ref[6] = { 1, 0, 0, 1, 0, 0 };
    for (int i = 0; i < 6; i++) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp[i]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ref[i]);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, col[i]); vkCmdDraw(cb, count[i], 1, first[i], 0);
    }
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, W, 1 } };
    vkCmdCopyImageToBuffer(cb, ci, VK_IMAGE_LAYOUT_GENERAL, rb.b, 1, &bic);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if ((r = cb_submit(cb))) { R(FAIL, N, "submission failed (%d)", r); goto out; }
    hbuf_sync(&rb, 0);
    {   // each quarter's inside (2 texels from its edges) in one colour, or 1 for mixed
        const uint32_t *p = rb.p; uint32_t q[4];
        for (int k = 0; k < 4; k++) { uint32_t x0 = (k & 1) ? 18 : 2, y0 = (k & 2) ? 18 : 2, c = p[y0 * W + x0];
            for (uint32_t y = y0; y < y0 + 12; y++) for (uint32_t x = x0; x < x0 + 12; x++) if (p[y * W + x] != c) c = 1;
            q[k] = c; }
        const uint32_t RED = 0xFF0000FFu, GREEN = 0xFF00FF00u, BLUE = 0xFFFF0000u, WHITE = 0xFFFFFFFFu, YELLOW = 0xFF00FFFFu;
        char why[256] = ""; size_t l;
        if (q[0] == 0 || q[1] == 0 || q[2] == 0 || q[3] == 0) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe triangle strip did not cover the target", l ? "; " : ""); }
        if (q[0] == GREEN && q[1] == GREEN) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe cull mode in the pipeline was ignored", l ? "; " : ""); }
        else if (!((q[0] == GREEN && q[1] == RED) || (q[0] == RED && q[1] == GREEN))) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sculling drew neither or mixed (top %#x / %#x)", l ? "; " : "", q[0], q[1]); }
        if (q[2] != BLUE) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sstencil EQUAL / depth LESS in front drew %#x, not blue", l ? "; " : "", q[2]); }
        if (q[3] == WHITE) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe depth test in the pipeline was ignored", l ? "; " : ""); }
        else if (q[3] == YELLOW) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe stencil test in the pipeline was ignored", l ? "; " : ""); }
        else if (q[3] != RED) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sbottom right %#x, not red", l ? "; " : "", q[3]); }
        if (!why[0]) R(PASS, N, "render pass in GENERAL layout and pipelines with cull, topology, depth, stencil and vertex stride built in drew right: the engine's fallback path works on this driver");
        else R(FAIL, N, "%s: the engine's stock-driver fallback would draw wrong here", why);
    }
out:
    for (int i = 0; i < 6; i++) if (pp[i]) vkDestroyPipeline(s_dev, pp[i], NULL);
    if (pl) vkDestroyPipelineLayout(s_dev, pl, NULL); if (vm) vkDestroyShaderModule(s_dev, vm, NULL); if (fm) vkDestroyShaderModule(s_dev, fm, NULL);
    hbuf_free(&vbuf); hbuf_free(&rb);
    if (fb) vkDestroyFramebuffer(s_dev, fb, NULL); if (rp) vkDestroyRenderPass(s_dev, rp, NULL);
    if (cv) vkDestroyImageView(s_dev, cv, NULL); if (dv) vkDestroyImageView(s_dev, dv, NULL);
    if (ci) vkDestroyImage(s_dev, ci, NULL); if (di) vkDestroyImage(s_dev, di, NULL); if (cm) vkFreeMemory(s_dev, cm, NULL); if (dm) vkFreeMemory(s_dev, dm, NULL);
}

// ---- dma-buf heap route ----
struct st_heap_alloc { uint64_t len; uint32_t fd, fd_flags; uint64_t heap_flags; };
#define ST_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct st_heap_alloc)
// CPU writes the first half at view, the GPU copies it out and fills the second half; both directions checked, twice. NULL = pass
static const char *roundtrip(VkBuffer a, VkDeviceSize size, volatile uint32_t *view) {
    VkDeviceSize half = size / 2; hbuf rb; const char *res = NULL;
    if (hbuf_make(&rb, half, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) return "no readback buffer";
    for (int round = 0; round < 2 && !res; round++) {
        uint32_t salt = 0x9e3779b9u * (uint32_t)(round + 1), fill = 0xCAFE0000u + (uint32_t)round;
        for (VkDeviceSize i = 0; i < half / 4; i++) view[i] = (uint32_t)i ^ salt;
        VkCommandBuffer cb = cb_begin(); VkBufferCopy cp = { 0, 0, half };
        vkCmdCopyBuffer(cb, a, rb.b, 1, &cp); vkCmdFillBuffer(cb, a, half, half, fill); barrier_host(cb);
        if (cb_submit(cb)) { res = "submission failed"; break; }
        hbuf_sync(&rb, 0); const uint32_t *rp = rb.p;
        for (VkDeviceSize i = 0; !res && i < half / 4; i++) if (rp[i] != ((uint32_t)i ^ salt)) res = "the GPU did not see the CPU's writes";
        for (VkDeviceSize i = half / 4; !res && i < size / 4; i++) if (view[i] != fill) res = "the CPU did not see the GPU's writes";
    }
    hbuf_free(&rb); return res;
}
static void t_dmabuf(void) {
    const char *N = "dmabuf-heap";
    if (!s_ext_fd || !s_ext_dmabuf || !p_fdprops) { R(SKIP, N, "the driver imports no dma-bufs (VK_EXT_external_memory_dma_buf missing): the engine moves the driver's own mappings instead"); return; }
    VkPhysicalDeviceExternalBufferInfo xi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkExternalBufferProperties xp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(s_pd, &xi, &xp);
    unsigned feat = (unsigned)xp.externalMemoryProperties.externalMemoryFeatures;
    O("  dma-buf buffer features %#x (import %s, export %s)\n", feat, feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? "yes" : "no", feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "yes" : "no");
    if (!(feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) { R(WARN, N, "the driver lists dma-buf buffers as not importable: the engine keeps its memory pools on another route (more memory, slower)"); return; }
    static const char *heaps[] = { "/dev/dma_heap/system-uncached", "/dev/dma_heap/system" }; int hfd = -1, err = 0; const char *hname = NULL;
    for (int i = 0; i < 2 && hfd < 0; i++) { if ((hfd = open(heaps[i], O_RDONLY | O_CLOEXEC)) >= 0) hname = heaps[i]; else err = errno; }
    if (hfd < 0) { R(SKIP, N, "no dma-buf heap opens for an app (errno %d): the engine falls back the same way", err); return; }
    char notes[512] = ""; int fails = 0;
    static const VkDeviceSize sizes[] = { 4ull << 20, 64ull << 20 };
    for (int s = 0; s < 2; s++) {
        VkDeviceSize size = sizes[s]; const char *res = NULL;
        struct st_heap_alloc d = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
        void *at = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); int fd = -1;
        VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE;
        if (at == MAP_FAILED) { at = NULL; res = "no address space"; }
        else if (ioctl(hfd, ST_HEAP_IOCTL_ALLOC, &d) < 0) res = "the heap refused the allocation";
        else if ((fd = (int)d.fd), mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != at) res = "mapping the dma-buf at the chosen address failed";
        else {
            VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
            VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
            VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
            VkResult r;
            if ((r = p_fdprops(s_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp))) res = "vkGetMemoryFdPropertiesKHR failed";
            else if ((r = vkCreateBuffer(s_dev, &bi, NULL, &b))) { b = VK_NULL_HANDLE; res = "the external buffer was refused"; }
            else {
                VkMemoryRequirements rq; vkGetBufferMemoryRequirements(s_dev, b, &rq);
                int t = mem_type(rq.memoryTypeBits & fp.memoryTypeBits, 0, 0); int dupfd = dup(fd);
                VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dupfd };
                VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size, .memoryTypeIndex = (uint32_t)t };
                if (t < 0) { res = "no memory type takes this dma-buf"; close(dupfd); }
                else if ((r = vkAllocateMemory(s_dev, &ai, NULL, &m))) { m = VK_NULL_HANDLE; res = "the driver refused the dma-buf import"; close(dupfd); }
                else if ((r = vkBindBufferMemory(s_dev, b, m, 0))) res = "binding the imported memory failed";
                else res = roundtrip(b, size, (volatile uint32_t *)at);
            }
        }
        if (b) vkDestroyBuffer(s_dev, b, NULL); if (m) vkFreeMemory(s_dev, m, NULL); if (at) munmap(at, size); if (fd >= 0) close(fd);
        size_t l = strlen(notes); snprintf(notes + l, sizeof notes - l, "%s%llu MB: %s", l ? "; " : "", (unsigned long long)(size >> 20), res ? res : "pass");
        if (res) fails++;
    }
    close(hfd);
    if (!fails) R(PASS, N, "%s mapped at a fixed address and imported: GPU and CPU see each other's writes (%s)", hname, notes);
    else R(FAIL, N, "%s: %s; the engine's heap route falls back to mirrored pools here (twice the memory, stalls)", hname, notes);
}

// ---- BC textures: the engine's BC layer, linked as is ----
static void bc_log(const char *fmt, ...) { O("    bc_gpu: "); va_list ap; va_start(ap, fmt); probe_vout(fmt, ap); va_end(ap); O("\n"); }
static void t_bc(void) {
    const char *N = "bc-textures";
    if (s_feat.textureCompressionBC) { R(PASS, N, "BC1-BC7 sampled natively (textureCompressionBC): the game's textures upload as they are"); return; }
    static const struct { int k; const char *n; } K[] = { {1,"BC1"}, {2,"BC2"}, {3,"BC3"}, {4,"BC4"}, {-4,"BC4s"}, {5,"BC5"}, {-5,"BC5s"}, {6,"BC6H"}, {-6,"BC6Hs"}, {7,"BC7"} };
    int etc = s_feat.textureCompressionETC2, mode = etc ? 1 : 0;   // nvn_vk.c: re-encoded (ETC2 / EAC / ASTC) where the GPU has ETC2, else decoded to plain texels
    O("  no BC on this GPU: textures are %s (ETC2 %d, ASTC LDR %d)\n", etc ? "re-encoded to ETC2 / EAC / ASTC (the same size as BC)" : "decoded to plain texels (4-8 times the memory)",
      etc, s_feat.textureCompressionASTC_LDR);
    bc_gpu_info bi = { s_dev, s_q, s_qf, vkGetDeviceProcAddr, &s_mp, bc_log };
    int on = bc_gpu_init(&bi);
    char gpu[160] = "", cpu[160] = ""; int ncpu = 0;
    for (size_t i = 0; i < sizeof K / sizeof *K; i++) { char *l = on && bc_gpu_can(K[i].k, mode) ? gpu : (ncpu++, cpu); size_t n = strlen(l); snprintf(l + n, 160 - n, "%s%s", n ? " " : "", K[i].n); }
    if (!on) R(WARN, N, "no BC kind converts on this GPU bit for bit with the CPU: every BC texture converts on the CPU (longer loads, more stutter)");
    else if (ncpu) R(WARN, N, "on the GPU: %s; on the CPU (slower): %s", gpu, cpu);
    else R(PASS, N, "every BC kind converts on the GPU, bit for bit with the CPU");
}

static void meminfo(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char ln[256]; long total = 0, avail = 0;
    if (f) { while (fgets(ln, sizeof ln, f)) { sscanf(ln, "MemTotal: %ld kB", &total); sscanf(ln, "MemAvailable: %ld kB", &avail); } fclose(f); }
    // the size on the box: the kernel reports less (firmware and GPU carve-outs), so round up to the next size phones are sold with
    static const long sold[] = { 2, 3, 4, 6, 8, 12, 16, 24, 32 }; long gb = 0;
    for (size_t i = 0; i < sizeof sold / sizeof *sold && !gb; i++) if (total <= sold[i] * 1024 * 1024) gb = sold[i];
    if (!gb) gb = (total + 1024 * 1024 - 1) / (1024 * 1024);
    R(INFO, "memory", "%ld GB phone (%ld MB seen by Android), %ld MB available now: %s", gb, total / 1024, avail / 1024,
      gb >= 8 ? "enough for Galar Native" : gb >= 6 ? "tight for Galar Native (Low graphics and Half textures help)" : "below Galar Native's needs: Android may close the game");
}

// The engine's hard requirements (nvn_vk.c mtl_init): a driver older than Vulkan 1.3 needs these extensions, or the game stops at start (VDIE)
static void t_requirements(void) {
    const char *N = "engine-requirements";
    if (s_api >= VK_API_VERSION_1_3) { R(PASS, N, "Vulkan 1.3 driver: dynamic rendering, timeline semaphores and extended dynamic state are core"); return; }
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, NULL); VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, ex);
    int timeline = s_api >= VK_API_VERSION_1_2 || has_ext(ex, ne, "VK_KHR_timeline_semaphore");
    int dynren = has_ext(ex, ne, "VK_KHR_dynamic_rendering"), eds = has_ext(ex, ne, "VK_EXT_extended_dynamic_state");
    free(ex);
    unsigned ma = VK_API_VERSION_MAJOR(s_api), mi = VK_API_VERSION_MINOR(s_api);
    if (!timeline) { R(FAIL, N, "Vulkan %u.%u driver without timeline semaphores: Galar Native cannot run on this driver (no fallback)", ma, mi); return; }
    if (dynren && eds) { R(PASS, N, "Vulkan %u.%u driver with every extension the engine needs", ma, mi); return; }
    // Since the stock-driver fallbacks (render passes; draw state built into pipelines) the engine runs without these two; v0.53 and older stopped at start
    R(WARN, N, "Vulkan %u.%u driver without%s%s%s: the engine uses its fallback%s (%s%s%s); Galar Native v0.53 and older stop at start here", ma, mi,
      dynren ? "" : " KHR_dynamic_rendering", !dynren && !eds ? " and" : "", eds ? "" : " EXT_extended_dynamic_state", !dynren && !eds ? "s" : "",
      dynren ? "" : "render passes", !dynren && !eds ? ", " : "", eds ? "" : "draw state built into each pipeline: no pipeline prewarm, more pipelines");
}

typedef void (*test_fn)(void);
void probe_selftests(void) {
    O("\n== Galar Native GPU self-tests (synthetic data: no game files)\n");
    memset(g_count, 0, sizeof g_count);
    meminfo();
    if (st_device()) return;
    O("  GPU %s, Vulkan %u.%u.%u, driver version %#x, vendor %#x device %#x\n", s_props.deviceName, VK_API_VERSION_MAJOR(s_props.apiVersion), VK_API_VERSION_MINOR(s_props.apiVersion),
      VK_API_VERSION_PATCH(s_props.apiVersion), s_props.driverVersion, s_props.vendorID, s_props.deviceID);
    static const struct { test_fn f; const char *n; } T[] = { { t_requirements, "engine-requirements" }, { t_floatctl, "float-controls" }, { t_shmath, "shader-maths" }, { t_barrier, "end-of-submit-barrier" }, { t_reinterp, "reinterpreted-format" },
                                                            { t_fallback, "fallback-draw" }, { t_dmabuf, "dmabuf-heap" }, { t_bc, "bc-textures" },
                                                            { t_3d_slices, "3d-slices" }, { t_3d_layered, "3d-layered" } };   // (3D last: it crashed Xclipse 540's driver)
    size_t nt = sizeof T / sizeof *T, i;
    guard(1);
    for (i = 0; i < nt; i++) {
        if (sigsetjmp(g_jb, 1)) { const char *st = g_step; R(FAIL, T[i].n, "the driver crashed during this test (signal %d%s%s): the game would close at the same point", (int)g_sig, st ? ", while " : "", st ? st : ""); break; }
        g_step = NULL;
        T[i].f();
    }
    guard(0);
    for (size_t j = i + 1; j < nt && i < nt; j++) R(SKIP, T[j].n, "not run: the driver crashed in an earlier test");
    O("Self-tests: %d pass, %d warn, %d fail, %d skip\n", g_count[PASS], g_count[WARN], g_count[FAIL], g_count[SKIP]);
    // the device stays alive for the rest of the probe: the BC layer keeps its objects for the process, as in the engine
}
