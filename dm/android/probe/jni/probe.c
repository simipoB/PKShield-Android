// Android device probe: checks, on the player's phone, what the nxnative host needs before the Android port is built.
// Memory: page size, loading code the way loader.c does (anonymous RW pages, copy, mprotect to RX), large address-space reservations.
// Registers: whether the kernel keeps x18 across system calls and thread switches, what bionic leaves in x18 and the TLS registers.
// Vulkan: the GPU, its driver, the features and extensions the NVN replay will use, and the texture formats the games ship (BC, ASTC, depth).
// The report is plain text: the app shows it and offers Share / Copy so it can be sent back.
#include <jni.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <vulkan/vulkan.h>

static char *g_out; static size_t g_len, g_cap;
static void P(const char *fmt, ...) {
    char buf[4096]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n < 0) return; if (n >= (int)sizeof buf) n = sizeof buf - 1;
    if (g_len + n + 1 > g_cap) { g_cap = (g_len + n + 1) * 2; g_out = realloc(g_out, g_cap); }
    memcpy(g_out + g_len, buf, n); g_len += n; g_out[g_len] = 0;
}
static const char *prop(const char *k) { static char v[8][PROP_VALUE_MAX]; static int i; char *r = v[i++ & 7]; r[0] = 0; __system_property_get(k, r); return r[0] ? r : "-"; }

// ---- Device ----
static void device(void) {
    struct utsname u; uname(&u);
    P("== Device\n");
    P("model %s (%s %s), board %s\n", prop("ro.product.model"), prop("ro.product.manufacturer"), prop("ro.product.device"), prop("ro.product.board"));
    P("soc %s %s, Android %s (API %s), security patch %s\n", prop("ro.soc.manufacturer"), prop("ro.soc.model"), prop("ro.build.version.release"), prop("ro.build.version.sdk"), prop("ro.build.version.security_patch"));
    P("kernel %s %s\n", u.release, u.machine);
    FILE *f = fopen("/proc/meminfo", "r"); char ln[256];
    if (f) { for (int i = 0; i < 3 && fgets(ln, sizeof ln, f); i++) P("%s", ln); fclose(f); }
    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF); P("cpus %d:", ncpu);
    for (int c = 0; c < ncpu; c++) {
        char p[128]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        long khz = 0; FILE *g = fopen(p, "r"); if (g) { if (fscanf(g, "%ld", &khz) != 1) khz = 0; fclose(g); }
        P(" %ld", khz / 1000);
    }
    P(" MHz\n");
    f = fopen("/proc/cpuinfo", "r");
    if (f) { while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "Features", 8)) { P("%s", ln); break; } fclose(f); }
}

// ---- Memory ----
static sigjmp_buf g_jb; static volatile int g_sig;
static void on_sig(int s) { g_sig = s; siglongjmp(g_jb, 1); }
typedef int (*fn_t)(void);
static const uint32_t RET42[] = { 0x52800540u /* mov w0, #42 */, 0xd65f03c0u /* ret */ };

static int run_code(void *p) {
    struct sigaction sa = {0}, o1, o2; sa.sa_handler = on_sig; sigaction(SIGSEGV, &sa, &o1); sigaction(SIGILL, &sa, &o2);
    int r = -1; g_sig = 0;
    if (!sigsetjmp(g_jb, 1)) { __builtin___clear_cache((char *)p, (char *)p + 8); r = ((fn_t)p)(); }
    sigaction(SIGSEGV, &o1, 0); sigaction(SIGILL, &o2, 0);
    return r;
}
static void memory(void) {
    long pg = sysconf(_SC_PAGESIZE);
    P("\n== Memory\npage size %ld%s\n", pg, pg == 16384 ? " (16 KB kernel)" : "");
    // 1. loader.c's path: anonymous RW, copy, mprotect RX (16 KB aligned like the host)
    uint8_t *a = mmap(NULL, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(a, RET42, sizeof RET42);
    int mp = mprotect(a, 0x4000, PROT_READ | PROT_EXEC);
    int r = mp ? -1 : run_code(a);
    P("anon RW -> mprotect RX: mprotect %s, run %s\n", mp ? "DENIED" : "ok", r == 42 ? "ok (42)" : g_sig ? "SIGNAL" : "failed");
    // 2. re-open for writing after RX (the rewriter's runtime patches, hle stubs)
    mp = mprotect(a, 0x4000, PROT_READ | PROT_WRITE);
    if (!mp) { ((uint32_t *)a)[0] = 0x52800560u; /* mov w0, #43 */ mp = mprotect(a, 0x4000, PROT_READ | PROT_EXEC); }
    r = mp ? -1 : run_code(a);
    P("RX -> RW -> RX again: %s\n", r == 43 ? "ok (43)" : mp ? "mprotect DENIED" : "failed");
    munmap(a, 0x8000);
    // 3. RWX at once (not needed by the host, reported for completeness)
    a = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED) P("anon RWX: DENIED\n");
    else { memcpy(a, RET42, sizeof RET42); r = run_code(a); P("anon RWX: %s\n", r == 42 ? "ok" : "failed"); munmap(a, 0x4000); }
    // 4. dual mapping through memfd: one RW view and one RX view of the same pages (lets the host write code without flipping protections)
    int fd = (int)syscall(SYS_memfd_create, "probe", 0);
    if (fd < 0) P("memfd dual map: memfd_create DENIED\n");
    else {
        ftruncate(fd, 0x4000);
        uint8_t *w = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), *x = mmap(NULL, 0x4000, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (w == MAP_FAILED || x == MAP_FAILED) P("memfd dual map: RX view DENIED\n");
        else { memcpy(w, RET42, sizeof RET42); r = run_code(x); P("memfd dual map (RW + RX views): %s\n", r == 42 ? "ok" : "failed"); }
        if (w != MAP_FAILED) munmap(w, 0x4000); if (x != MAP_FAILED) munmap(x, 0x4000); close(fd);
    }
    // 5. address-space reservations (guest heap, NVN pools, Shield's 39-bit space)
    P("PROT_NONE reservations:");
    for (int gb = 4; gb <= 512; gb *= 2) {
        void *v = mmap(NULL, (size_t)gb << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        P(" %dG %s", gb, v == MAP_FAILED ? "no" : "ok"); if (v != MAP_FAILED) munmap(v, (size_t)gb << 30); else break;
    }
    P("\n");
    // highest address mmap hands out tells the kernel's VA size (39-bit = 512 GB user space, 48-bit = 256 TB)
    void *hi = mmap((void *)(1ull << 46), 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), *top = mmap(NULL, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    P("mmap hint 1<<46 -> %p, default top %p => user VA ~%d bits\n", hi, top, 64 - __builtin_clzll((uint64_t)(uintptr_t)top));
    if (hi != MAP_FAILED) munmap(hi, 0x4000); munmap(top, 0x4000);
    // low fixed addresses (Horizon's default code base 0x8000000)
    void *lo = mmap((void *)0x8000000, 0x10000000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    P("fixed 0x8000000 (256 MB): %s\n", lo == (void *)0x8000000 ? "ok" : "taken / refused");
    if (lo != MAP_FAILED) munmap(lo, 0x10000000);
}

// ---- Registers ----
static uint64_t rd_x18(void) { uint64_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }
static uint64_t rd_tpidr(void) { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static uint64_t rd_tpidrro(void) { uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v)); return v; }
// Put a value in x18, make many raw system calls (getpid, sched_yield, a 1 ms nanosleep: the thread leaves the CPU), then read x18 back.
// All in one asm block with svc, never through libc: bionic may use x18 as its shadow call stack pointer, so no libc call may run while x18 holds the test value
static uint64_t x18_survives(uint64_t magic) {
    static const struct timespec ms = {0, 1000000};
    uint64_t got;
    __asm__ volatile(
        "mov x9, x18\n mov x18, %1\n mov x10, #600\n"
        "1: mov x8, #172\n svc #0\n"            // getpid
        "   mov x8, #124\n svc #0\n"            // sched_yield
        "   mov x0, %2\n mov x1, #0\n mov x8, #101\n svc #0\n"   // nanosleep(1 ms)
        "   subs x10, x10, #1\n b.ne 1b\n"
        "mov %0, x18\n mov x18, x9\n"
        : "=r"(got) : "r"(magic), "r"(&ms) : "x0", "x1", "x8", "x9", "x10", "memory", "cc");
    return got;
}
static void *thr(void *p) { uint64_t *o = p; o[0] = rd_x18(); o[1] = rd_tpidr(); o[2] = x18_survives(0x5eed0000cafe1234ull); return 0; }
static void registers(void) {
    P("\n== Registers\nmain thread: x18 %#llx, tpidr_el0 %#llx, tpidrro_el0 %#llx\n", (unsigned long long)rd_x18(), (unsigned long long)rd_tpidr(), (unsigned long long)rd_tpidrro());
    uint64_t o[3]; pthread_t t; pthread_create(&t, 0, thr, o); pthread_join(t, 0);
    P("new thread: x18 %#llx%s, tpidr_el0 %#llx\n", (unsigned long long)o[0], o[0] ? " (bionic shadow call stack in use)" : " (zero: no shadow call stack)", (unsigned long long)o[1]);
    P("x18 kept across syscalls + yields: %s\n", o[2] == 0x5eed0000cafe1234ull ? "yes" : "NO");
}

// ---- Vulkan ----
static const char *WANT_EXT[] = {
    "VK_KHR_push_descriptor", "VK_KHR_dynamic_rendering", "VK_KHR_maintenance4", "VK_KHR_maintenance5", "VK_KHR_maintenance6",
    "VK_KHR_shader_float16_int8", "VK_KHR_sampler_mirror_clamp_to_edge", "VK_KHR_draw_indirect_count", "VK_KHR_timeline_semaphore",
    "VK_KHR_synchronization2", "VK_KHR_pipeline_library", "VK_EXT_graphics_pipeline_library", "VK_KHR_fragment_shader_barycentric",
    "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3", "VK_EXT_vertex_input_dynamic_state",
    "VK_EXT_transform_feedback", "VK_EXT_custom_border_color", "VK_EXT_robustness2", "VK_EXT_descriptor_indexing", "VK_EXT_descriptor_buffer",
    "VK_EXT_depth_clip_control", "VK_EXT_depth_clip_enable", "VK_EXT_depth_range_unrestricted", "VK_EXT_shader_stencil_export",
    "VK_EXT_shader_viewport_index_layer", "VK_EXT_provoking_vertex", "VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor",
    "VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8", "VK_EXT_primitive_topology_list_restart", "VK_EXT_line_rasterization",
    "VK_EXT_color_write_enable", "VK_EXT_conditional_rendering", "VK_EXT_sampler_filter_minmax", "VK_EXT_shader_demote_to_helper_invocation",
    "VK_EXT_subgroup_size_control", "VK_KHR_shader_draw_parameters", "VK_EXT_memory_budget", "VK_EXT_pipeline_creation_cache_control",
    "VK_KHR_external_memory_fd", "VK_ANDROID_external_memory_android_hardware_buffer", "VK_QCOM_render_pass_transform", "VK_EXT_attachment_feedback_loop_layout",
};
static void vulkan(void) {
    P("\n== Vulkan\n");
    uint32_t iv = VK_API_VERSION_1_0; vkEnumerateInstanceVersion(&iv);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative probe", .apiVersion = iv };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst; VkResult vr = vkCreateInstance(&ci, 0, &inst);
    P("loader %u.%u.%u, vkCreateInstance %d\n", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv), VK_API_VERSION_PATCH(iv), vr);
    if (vr) return;
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, 0); VkPhysicalDevice pd[4]; if (n > 4) n = 4; vkEnumeratePhysicalDevices(inst, &n, pd);
    for (uint32_t d = 0; d < n; d++) {
        VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceSubgroupProperties sg = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &drv };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
        vkGetPhysicalDeviceProperties2(pd[d], &p2);
        VkPhysicalDeviceProperties *p = &p2.properties; VkPhysicalDeviceLimits *L = &p->limits;
        P("GPU %s, Vulkan %u.%u.%u, driver %s / %s (%#x), vendor %#x device %#x\n", p->deviceName, VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion),
          drv.driverName, drv.driverInfo, p->driverVersion, p->vendorID, p->deviceID);
        P("subgroup %u; limits: 2D %u, 3D %u, layers %u, ubo %u, ssbo %u, push %u, sampled/stage %u, samplers/stage %u, storage img/stage %u, attachments %u, viewports %u, vtx attrs %u, vtx bindings %u\n",
          sg.subgroupSize, L->maxImageDimension2D, L->maxImageDimension3D, L->maxImageArrayLayers, L->maxUniformBufferRange, L->maxStorageBufferRange, L->maxPushConstantsSize,
          L->maxPerStageDescriptorSampledImages, L->maxPerStageDescriptorSamplers, L->maxPerStageDescriptorStorageImages, L->maxColorAttachments, L->maxViewports, L->maxVertexInputAttributes, L->maxVertexInputBindings);
        P("min ubo align %llu, min ssbo align %llu, timestamp period %.2f ns, sample counts %#x\n", (unsigned long long)L->minUniformBufferOffsetAlignment, (unsigned long long)L->minStorageBufferOffsetAlignment, L->timestampPeriod, L->framebufferColorSampleCounts);
        VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = p->apiVersion >= VK_API_VERSION_1_3 ? &f13 : 0 };
        VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = p->apiVersion >= VK_API_VERSION_1_2 ? &f11 : 0 };
        vkGetPhysicalDeviceFeatures2(pd[d], &f2); VkPhysicalDeviceFeatures *F = &f2.features;
#define FT(x) P(" %s%s", F->x ? "" : "-", #x)
        P("features:"); FT(textureCompressionBC); FT(textureCompressionASTC_LDR); FT(textureCompressionETC2); FT(geometryShader); FT(tessellationShader); FT(shaderFloat64); FT(shaderInt64); FT(shaderInt16);
        FT(depthClamp); FT(depthBiasClamp); FT(depthBounds); FT(logicOp); FT(dualSrcBlend); FT(independentBlend); FT(multiViewport); FT(wideLines); FT(largePoints); FT(fillModeNonSolid);
        FT(samplerAnisotropy); FT(imageCubeArray); FT(fragmentStoresAndAtomics); FT(vertexPipelineStoresAndAtomics); FT(shaderClipDistance); FT(shaderCullDistance); FT(multiDrawIndirect); FT(drawIndirectFirstInstance);
        FT(shaderStorageImageWriteWithoutFormat); FT(shaderStorageImageReadWithoutFormat); FT(shaderImageGatherExtended); FT(occlusionQueryPrecise); FT(pipelineStatisticsQuery); FT(sampleRateShading); FT(variableMultisampleRate);
        P("\n");
        if (f2.pNext) P("1.2: float16 %d int8 %d descIndexing %d timelineSem %d bufferDeviceAddress %d scalarBlock %d samplerMirrorClamp %d drawIndirectCount %d, 1.1: shaderDrawParameters %d\n",
                        f12.shaderFloat16, f12.shaderInt8, f12.descriptorIndexing, f12.timelineSemaphore, f12.bufferDeviceAddress, f12.scalarBlockLayout, f12.samplerMirrorClampToEdge, f12.drawIndirectCount, f11.shaderDrawParameters);
        if (p->apiVersion >= VK_API_VERSION_1_3) P("1.3: dynamicRendering %d synchronization2 %d maintenance4 %d demoteToHelper %d subgroupSizeControl %d\n", f13.dynamicRendering, f13.synchronization2, f13.maintenance4, f13.shaderDemoteToHelperInvocation, f13.subgroupSizeControl);
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd[d], 0, &ne, 0); VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(pd[d], 0, &ne, ex);
        P("extensions %u; wanted:", ne);
        for (size_t w = 0; w < sizeof WANT_EXT / sizeof *WANT_EXT; w++) { int has = 0; for (uint32_t e = 0; e < ne; e++) if (!strcmp(ex[e].extensionName, WANT_EXT[w])) has = 1; P(" %s%s", has ? "" : "-", WANT_EXT[w] + 3); }
        P("\nall:"); for (uint32_t e = 0; e < ne; e++) P(" %s", ex[e].extensionName + 3); P("\n"); free(ex);
        static const struct { VkFormat f; const char *n; } FM[] = {
            {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"}, {VK_FORMAT_BC2_UNORM_BLOCK, "BC2"}, {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"}, {VK_FORMAT_BC3_SRGB_BLOCK, "BC3s"}, {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"},
            {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"}, {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H"}, {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"}, {VK_FORMAT_BC7_SRGB_BLOCK, "BC7s"},
            {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC4x4"}, {VK_FORMAT_ASTC_8x8_SRGB_BLOCK, "ASTC8x8s"}, {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2"},
            {VK_FORMAT_D16_UNORM, "D16"}, {VK_FORMAT_X8_D24_UNORM_PACK32, "X8D24"}, {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8"}, {VK_FORMAT_D32_SFLOAT, "D32"}, {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"}, {VK_FORMAT_S8_UINT, "S8"},
            {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10"}, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "RGB10A2"}, {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"}, {VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F"},
            {VK_FORMAT_R16G16B16A16_UNORM, "RGBA16"}, {VK_FORMAT_R32_UINT, "R32UI"}, {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "RGB9E5"}, {VK_FORMAT_B5G6R5_UNORM_PACK16, "B5G6R5"}, {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1RGB5"},
            {VK_FORMAT_R8G8B8_UNORM, "RGB8"}, {VK_FORMAT_R16G16B16_SFLOAT, "RGB16F"}, {VK_FORMAT_R32G32B32_SFLOAT, "RGB32F"},
        };
        P("formats (S sampled, L linear filter, C color target, D depth target, B blend, W storage, V vertex):\n");
        for (size_t i = 0; i < sizeof FM / sizeof *FM; i++) {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd[d], FM[i].f, &fp); VkFormatFeatureFlags o = fp.optimalTilingFeatures;
            P(" %s:%s%s%s%s%s%s%s", FM[i].n, o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "S" : "", o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? "L" : "", o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? "C" : "",
              o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "D" : "", o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT ? "B" : "", o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? "W" : "",
              fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT ? "V" : "");
            if (!o && !fp.bufferFeatures) P("none");
        }
        P("\n");
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd[d], &mp);
        P("heaps:"); for (uint32_t h = 0; h < mp.memoryHeapCount; h++) P(" %u MB%s", (unsigned)(mp.memoryHeaps[h].size >> 20), mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (local)" : "");
        P("; types:"); for (uint32_t t = 0; t < mp.memoryTypeCount; t++) P(" %u:%#x", mp.memoryTypes[t].heapIndex, mp.memoryTypes[t].propertyFlags); P("\n");
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd[d], &nq, 0); VkQueueFamilyProperties q[8]; if (nq > 8) nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(pd[d], &nq, q);
        P("queues:"); for (uint32_t i = 0; i < nq; i++) P(" %u x %#x (ts %u bits)", q[i].queueCount, q[i].queueFlags, q[i].timestampValidBits); P("\n");
    }
    vkDestroyInstance(inst, 0);
}

// ---- Zero-copy GPU memory at a chosen address (probe 2) ----
// NVN memory pools are game memory the GPU reads in place (on Apple: a no-copy MTLBuffer over the game's pages). Each route below puts GPU-visible memory at a fixed address picked
// in advance (as the replay would at the game's pool address), then checks both directions: the GPU copies what the CPU wrote there, and the CPU sees what the GPU filled.
static VkDevice g_dev; static VkQueue g_q; static VkCommandPool g_cp; static uint32_t g_qf; static VkPhysicalDeviceMemoryProperties g_mp; static VkPhysicalDevice g_pd;
static int has_dext(const char *n) { uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pd, 0, &ne, 0); VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(g_pd, 0, &ne, ex);
    int h = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(ex[i].extensionName, n)) h = 1; free(ex); return h; }
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want) { for (uint32_t i = 0; i < g_mp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_mp.memoryTypes[i].propertyFlags & want) == want) return (int)i; return -1; }
// Both directions on [mem, mem + size) seen by the CPU at view: 0 = pass
static const char *gpu_cpu_check(VkDeviceMemory mem, VkDeviceSize size, volatile uint32_t *view) {
    VkDeviceSize half = size / 2;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer a, r; if (vkCreateBuffer(g_dev, &bi, 0, &a)) return "buffer";
    if (vkBindBufferMemory(g_dev, a, mem, 0)) return "bind";
    bi.size = half; vkCreateBuffer(g_dev, &bi, 0, &r); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, r, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory rm; vkAllocateMemory(g_dev, &ai, 0, &rm); vkBindBufferMemory(g_dev, r, rm, 0); uint32_t *rp; vkMapMemory(g_dev, rm, 0, half, 0, (void **)&rp);
    const char *res = 0;
    for (int round = 0; round < 2 && !res; round++) {
        uint32_t salt = 0x9e3779b9u * (uint32_t)(round + 1);
        for (VkDeviceSize i = 0; i < half / 4; i++) view[i] = (uint32_t)i ^ salt;            // CPU writes the first half
        VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb; vkAllocateCommandBuffers(g_dev, &ca, &cb);
        VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cb, &cbi);
        VkBufferCopy cp = { 0, 0, half }; vkCmdCopyBuffer(cb, a, r, 1, &cp);                 // GPU reads it
        vkCmdFillBuffer(cb, a, half, half, 0xCAFE0000u + (uint32_t)round);                     // GPU writes the second half
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, 0, 0, 0);
        vkEndCommandBuffer(cb);
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f; vkCreateFence(g_dev, &fi, 0, &f);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        if (vkQueueSubmit(g_q, 1, &si, f) || vkWaitForFences(g_dev, 1, &f, 1, 5000000000ull)) res = "submit / wait";
        vkDestroyFence(g_dev, f, 0); vkFreeCommandBuffers(g_dev, g_cp, 1, &cb);
        for (VkDeviceSize i = 0; !res && i < half / 4; i++) if (rp[i] != ((uint32_t)i ^ salt)) res = "GPU did not see the CPU's writes";
        for (VkDeviceSize i = half / 4; !res && i < size / 4; i++) if (view[i] != 0xCAFE0000u + (uint32_t)round) res = "CPU did not see the GPU's writes";
    }
    vkDestroyBuffer(g_dev, a, 0); vkDestroyBuffer(g_dev, r, 0); vkUnmapMemory(g_dev, rm); vkFreeMemory(g_dev, rm, 0);
    return res;
}
static void *reserve(size_t n) { void *p = mmap(0, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? 0 : p; }
static VkDeviceMemory alloc_buf_mem(VkDeviceSize size, VkMemoryPropertyFlags want, const void *pnext, int *type) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer b; vkCreateBuffer(g_dev, &bi, 0, &b); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, b, &rq); vkDestroyBuffer(g_dev, b, 0);
    *type = mem_type(rq.memoryTypeBits, want); if (*type < 0) return VK_NULL_HANDLE;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = pnext, .allocationSize = size, .memoryTypeIndex = (uint32_t)*type };
    VkDeviceMemory m; return vkAllocateMemory(g_dev, &ai, 0, &m) ? VK_NULL_HANDLE : m;
}
#include <android/hardware_buffer.h>
#include <sys/socket.h>
#include <vulkan/vulkan_android.h>
static void zero_copy(void) {
    P("\n== Zero-copy GPU memory at a fixed address\n");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; if (vkCreateInstance(&ici, 0, &inst)) { P("no instance\n"); return; }
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &g_pd); if (!n) { P("no GPU\n"); return; }
    vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, 0); VkQueueFamilyProperties qp[8]; if (nq > 8) nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    for (g_qf = 0; g_qf < nq && !(qp[g_qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_qf++) {}
    const char *want[] = { "VK_KHR_external_memory_fd", "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign", "VK_KHR_sampler_ycbcr_conversion", "VK_EXT_external_memory_host" };
    const char *on[8]; uint32_t non = 0; int has_fd = 0, has_ahb = 0, has_host = 0;
    for (int i = 0; i < 5; i++) if (has_dext(want[i])) { on[non++] = want[i]; if (i == 0) has_fd = 1; if (i == 1) has_ahb = 1; if (i == 4) has_host = 1; }
    float prio = 1; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = non, .ppEnabledExtensionNames = on };
    if (vkCreateDevice(g_pd, &dci, 0, &g_dev)) { P("device creation failed\n"); return; }
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qf }; vkCreateCommandPool(g_dev, &pci, 0, &g_cp);
    P("external memory: opaque fd %s, Android hardware buffer %s, host pointer %s\n", has_fd ? "yes" : "no", has_ahb ? "yes" : "no", has_host ? "yes" : "no");
    const VkMemoryPropertyFlags HVC = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, HVCC = HVC | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (int si = 0; si < 2; si++) {
        VkDeviceSize size = si ? 64ull << 20 : 1ull << 20; int t;
        P("-- %llu MB\n", (unsigned long long)(size >> 20));
        // 0. baseline: Vulkan's own mapping (checks the harness)
        { VkDeviceMemory m = alloc_buf_mem(size, HVCC, 0, &t); if (!m) m = alloc_buf_mem(size, HVC, 0, &t); void *p = 0;
          if (!m || vkMapMemory(g_dev, m, 0, size, 0, &p)) P("baseline: allocation failed\n");
          else { const char *r = gpu_cpu_check(m, size, p); P("baseline (vkMapMemory, type %d flags %#x): %s\n", t, g_mp.memoryTypes[t].propertyFlags, r ? r : "pass"); vkUnmapMemory(g_dev, m); }
          if (m) vkFreeMemory(g_dev, m, 0); }
        // C. mremap: Vulkan's mapping moved to the chosen address
        { VkDeviceMemory m = alloc_buf_mem(size, HVCC, 0, &t); if (!m) m = alloc_buf_mem(size, HVC, 0, &t); void *p = 0, *at = reserve(size);
          if (!m || !at || vkMapMemory(g_dev, m, 0, size, 0, &p)) P("C mremap: setup failed\n");
          else { void *q = mremap(p, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, at);
                 if (q != at) P("C mremap of the Vulkan mapping: refused (errno %d)\n", errno);
                 else { const char *r = gpu_cpu_check(m, size, at); P("C mremap of the Vulkan mapping: %s\n", r ? r : "pass"); } }
          if (m) vkFreeMemory(g_dev, m, 0); }
        // B. opaque fd export, mapped at the chosen address
        if (has_fd) {
            VkExportMemoryAllocateInfo ex = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
            VkDeviceMemory m = alloc_buf_mem(size, HVCC, &ex, &t); if (!m) m = alloc_buf_mem(size, HVC, &ex, &t); int fd = -1; void *at = reserve(size);
            PFN_vkGetMemoryFdKHR gfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_dev, "vkGetMemoryFdKHR");
            VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = m, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
            if (!m || !at || !gfd || gfd(g_dev, &gi, &fd) || fd < 0) P("B opaque fd: export failed\n");
            else { void *q = mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
                   if (q != at) P("B opaque fd mmap: refused (errno %d)\n", errno);
                   else { const char *r = gpu_cpu_check(m, size, at); P("B opaque fd mmap: %s\n", r ? r : "pass"); }
                   close(fd); }
            if (m) vkFreeMemory(g_dev, m, 0);
        }
        // A. Android hardware buffer (BLOB): its dma-buf received through AHardwareBuffer_sendHandleToUnixSocket, mapped at the chosen address, imported into Vulkan
        if (has_ahb) {
            AHardwareBuffer_Desc d = { .width = (uint32_t)size, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
                .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER };
            AHardwareBuffer *hb = 0; int sv[2] = {-1, -1}, fd = -1; void *at = reserve(size);
            if (AHardwareBuffer_allocate(&d, &hb)) P("A hardware buffer: allocation failed\n");
            else if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || AHardwareBuffer_sendHandleToUnixSocket(hb, sv[0])) P("A hardware buffer: handle send failed\n");
            else {
                char data[4096], ctl[CMSG_SPACE(sizeof(int) * 16)]; struct iovec io = { data, sizeof data }; struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
                ssize_t k = recvmsg(sv[1], &mh, 0); int nfd = 0;
                for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) if (c->cmsg_type == SCM_RIGHTS) { nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)); memcpy(&fd, CMSG_DATA(c), sizeof fd); }
                P("A hardware buffer: handle message %zd bytes, %d fds\n", k, nfd);
                void *q = fd >= 0 && at ? mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) : MAP_FAILED;
                if (q != at) P("A hardware buffer mmap: refused (errno %d)\n", errno);
                else {
                    PFN_vkGetAndroidHardwareBufferPropertiesANDROID gp = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(g_dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
                    VkAndroidHardwareBufferPropertiesANDROID hp = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
                    if (!gp || gp(g_dev, hb, &hp)) P("A import: properties failed\n");
                    else {
                        VkImportAndroidHardwareBufferInfoANDROID imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = hb };
                        int ti = mem_type(hp.memoryTypeBits, 0); VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = hp.allocationSize, .memoryTypeIndex = (uint32_t)ti };
                        VkDeviceMemory m; VkResult vr = vkAllocateMemory(g_dev, &ai, 0, &m);
                        if (vr) P("A import: vkAllocateMemory %d\n", vr);
                        else { const char *r = gpu_cpu_check(m, size, at); P("A hardware buffer mmap + import (type %d flags %#x): %s\n", ti, g_mp.memoryTypes[ti].propertyFlags, r ? r : "pass"); vkFreeMemory(g_dev, m, 0); }
                    }
                }
                if (fd >= 0) close(fd);
            }
            if (sv[0] >= 0) { close(sv[0]); close(sv[1]); }
            if (hb) AHardwareBuffer_release(hb);
        }
    }
    vkDestroyCommandPool(g_dev, g_cp, 0); vkDestroyDevice(g_dev, 0); vkDestroyInstance(inst, 0);
}

JNIEXPORT jstring JNICALL Java_com_nxnative_probe_Probe_run(JNIEnv *env, jclass c) {
    (void)c; g_len = 0;
    P("nxnative Android probe 2\n"); device(); memory(); registers(); zero_copy(); vulkan();
    jstring s = (*env)->NewStringUTF(env, g_out ? g_out : "");
    return s;
}
