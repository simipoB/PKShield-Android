// Vulkan replay layer of the NVN null device: the interface of the Metal one (nvn_mtl.h, nvn_obj.h) on Vulkan, for Android (Adreno, Mali) and, through MoltenVK, the Mac
// (where it is checked frame by frame against the Metal replay). Built instead of nvn_mtl.m (dm/android/Makefile.host, dm/host/Makefile GFX=vulkan).
// Same model as nvn_mtl.m, written for correctness first:
//  - memory pools are GPU buffers over the game's own pages (zero copy): imported host memory where the driver offers it (VK_EXT_external_memory_host, MoltenVK), else the driver's
//    cached coherent memory moved onto the pool's address with mremap (Android: checked on Adreno 830 by the device probe, dm/android/probe);
//  - one command buffer per submission plus one for its texture uploads, submitted together; a timeline semaphore carries the fence values, a completion thread applies the
//    CPU-visible effects (events, counters) in order once the GPU is done;
//  - textures as in nvn_mtl.m: CPU-filled ones uploaded (deswizzled on the CPU) and re-uploaded when their pages change, render targets on the GPU, shared by storage and description;
//    every image stays in the GENERAL layout, and passes / copies are separated by full memory barriers;
//  - shaders are the translator's SPIR-V (<key>.spv) with the build's reflection (<key>.vkr, dm/android/tools/vk_meta.py); each stage's bindings move to their own range of one
//    push-descriptor set; dynamic rendering, dynamic state for everything the extended dynamic state covers.
#include <zlib.h>
#ifdef __ANDROID__
#include <malloc.h>
#endif
#define VK_NO_PROTOTYPES   // every Vulkan function comes from the driver opened at start (vk_dispatch.h, vk_open)
#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#endif
#include <dlfcn.h>
#include "vk_dispatch.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include "nvn_hmap.h"
#include "nvn_spv.h"
#include "nvn_ovl.h"
#include "nvn_dirty.h"
#include "../tex/nvn_fmt_vk.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/bc_decode.h"
#include "dmsh_chain.h"
#include "game_profile.h"
#include "../../host/dm.h"

#define VLOG(...) do { fprintf(stderr, "[dm] [nvn-vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VDIE(...) do { VLOG("FATAL " __VA_ARGS__); abort(); } while (0)
static int g_red_once[128];
#define RED_ONCE(k, ...) do { if (!g_red_once[k]++) VLOG("[RED] " __VA_ARGS__); } while (0)
#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) VDIE("%s failed: %d (%s:%d)", #x, r_, __FILE__, __LINE__); } while (0)
#define NTEX 256

uint64_t guest_ns(void);
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// ================= Device =================
static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkPhysicalDeviceProperties g_props; static VkPhysicalDeviceMemoryProperties g_memp;
static int g_has_swapchain, g_host_import, g_clipctl, g_border, g_divisor, g_eds2, g_moltenvk;
static VkDeviceSize g_host_align = 4096;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
// Linux / Android pools: the driver's memory exported as a file descriptor and mapped a second time over the pool's pages (g_fdmap 1; -1 until the first pool tries it,
// 0 when the driver has no such export or its mmap fails: then the mremap route below)
static int g_fdmap = 0;   // queue submissions come from the submit thread and the present thread
static PFN_vkCmdPushDescriptorSetKHR p_push;
static int g_nopush;   // no VK_KHR_push_descriptor (the emulator's gfxstream, some phones): each draw's bindings go into a descriptor set from the submission's pools
static uint32_t g_maxpush = 32;   // maxPushDescriptors: a program with more descriptors takes the descriptor-set path too (vprog np)
static int g_np;   // the program being bound uses descriptor sets (g_nopush, or over g_maxpush)
static PFN_vkGetMemoryHostPointerPropertiesEXT p_hostprops;
static PFN_vkGetImageSubresourceLayout2EXT p_layout2;   // VK_EXT_image_compression_control: whether an image's framebuffer compression (UBWC, AFBC) stayed on

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static void completion_start(void);
static void dummies_init(void);
static void pcache_open(void); static void *prewarm_main(void *arg);   // pipeline list and cache (below)
// The Vulkan driver: DM_VK_DRIVER=<folder>/<library> (an Adreno driver such as Mesa Turnip, chosen on the setup screen) is opened through libadrenotools
// (BSD-2-Clause, shipped in the app with its hook libraries); without it, or when that fails, the system's libvulkan
static void *vk_open(void) {
#ifdef __ANDROID__
    const char *drv = getenv("DM_VK_DRIVER");
    if (drv && *drv) {
        char libdir[1024] = "", dir[1024]; Dl_info di;
        if (dladdr((void *)vk_open, &di) && di.dli_fname) { snprintf(libdir, sizeof libdir, "%s", di.dli_fname); char *e = strrchr(libdir, '/'); if (e) e[1] = 0; }   // the app's native library folder
        snprintf(dir, sizeof dir, "%s", drv); char *name = strrchr(dir, '/');
        void *at = dlopen("libadrenotools.so", RTLD_NOW);
        typedef void *(*open_fn)(int, int, const char *, const char *, const char *, const char *, const char *, void **);
        open_fn op = at ? (open_fn)dlsym(at, "adrenotools_open_libvulkan") : NULL;
        if (op && name) {
            char *lib = name + 1; *name = 0;   // folder / library name
            char ddir[1030]; snprintf(ddir, sizeof ddir, "%s/", dir);
            void *h = op(RTLD_NOW, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */, NULL, libdir, ddir, lib, NULL, NULL);
            if (h) { VLOG("Vulkan driver %s (opened through libadrenotools)", drv); return h; }
        }
        VLOG("[RED] Vulkan driver %s could not be opened (libadrenotools %s): the system driver is used", drv, op ? "failed" : "missing");
    }
    return dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#else
    void *h = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL); if (!h) h = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL); if (!h) h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return h;
#endif
}
#define VK_LOAD_G(f) f = (PFN_##f)vkGetInstanceProcAddr(NULL, #f);
#define VK_LOAD_I(f) f = (PFN_##f)vkGetInstanceProcAddr(g_inst, #f);
static uint32_t g_api;   // the device's Vulkan version (DM_VK_API=11 / 12 tries an older device's path on a newer one)
static PFN_vkVoidFunction vk_dev_fn(const char *name) {   // a device function; before 1.3 the KHR / EXT name of a promoted one (dynamic rendering, timeline semaphores, extended dynamic state)
    char alt[128];
    if (g_api < VK_API_VERSION_1_3) for (int k = 0; k < 2; k++) { snprintf(alt, sizeof alt, "%s%s", name, k ? "EXT" : "KHR"); PFN_vkVoidFunction f = vkGetDeviceProcAddr(g_dev, alt); if (f) return f; }
    return vkGetDeviceProcAddr(g_dev, name);
}
#define VK_LOAD_D(f) f = (PFN_##f)vk_dev_fn(#f);
void mtl_init(void) {
    if (g_dev) return;
    if (!vkGetInstanceProcAddr) {
        void *lib = vk_open(); if (!lib) VDIE("no Vulkan driver could be opened");
        vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"); if (!vkGetInstanceProcAddr) VDIE("the Vulkan driver has no vkGetInstanceProcAddr");
        VK_GLOBAL(VK_LOAD_G)
    }
    uint32_t instance_api = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (enumerate_instance_version && enumerate_instance_version(&instance_api) != VK_SUCCESS) instance_api = VK_API_VERSION_1_0;
    if (instance_api > VK_API_VERSION_1_3) instance_api = VK_API_VERSION_1_3;
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = instance_api };
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof *ie); vkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
    const char *iext[6]; uint32_t niext = 0; VkInstanceCreateFlags ifl = 0;
    if (has_ext(ie, nie, "VK_KHR_portability_enumeration")) { iext[niext++] = "VK_KHR_portability_enumeration"; ifl |= 0x00000001; /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */ }
    if (has_ext(ie, nie, "VK_KHR_get_physical_device_properties2")) iext[niext++] = "VK_KHR_get_physical_device_properties2";
    if (has_ext(ie, nie, "VK_KHR_surface")) iext[niext++] = "VK_KHR_surface";
#ifdef __ANDROID__
    if (has_ext(ie, nie, "VK_KHR_android_surface")) iext[niext++] = "VK_KHR_android_surface";
#endif
    free(ie);
    const char *layers[1]; uint32_t nlayers = 0;
    if (getenv("DM_VK_VALIDATION")) layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = ifl, .pApplicationInfo = &ai, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext,
                                 .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers };
    VKCHECK(vkCreateInstance(&ici, NULL, &g_inst));
    VK_INSTANCE(VK_LOAD_I)
    uint32_t npd = 0; vkEnumeratePhysicalDevices(g_inst, &npd, NULL); if (!npd) VDIE("no Vulkan device");
    VkPhysicalDevice pds[8]; if (npd > 8) npd = 8; vkEnumeratePhysicalDevices(g_inst, &npd, pds); g_pd = pds[0];
    vkGetPhysicalDeviceProperties(g_pd, &g_props); vkGetPhysicalDeviceMemoryProperties(g_pd, &g_memp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    for (g_qf = 0; g_qf < nq && (qp[g_qf].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT); g_qf++) {}
    if (g_qf == nq) VDIE("no graphics + compute queue");
    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nde, de);
    const char *dext[24]; uint32_t ndext = 0;
    g_api = g_props.apiVersion;
    if (getenv("DM_VK_API")) { int a = atoi(getenv("DM_VK_API")); uint32_t f = a == 11 ? VK_API_VERSION_1_1 : a == 12 ? VK_API_VERSION_1_2 : g_api; if (f < g_api) g_api = f; }
    int old = g_api < VK_API_VERSION_1_3, old11 = g_api < VK_API_VERSION_1_2;
    if (old) {   // what Vulkan 1.3 has built in, as extensions (Vulkan 1.1 / 1.2 drivers: older Mali, PowerVR, Adreno 6xx stock drivers)
        static const char *need[] = { "VK_KHR_dynamic_rendering", "VK_KHR_timeline_semaphore", "VK_EXT_extended_dynamic_state" };
        static const char *deps11[] = { "VK_KHR_depth_stencil_resolve", "VK_KHR_create_renderpass2", "VK_KHR_multiview", "VK_KHR_maintenance2" };   // dynamic rendering's prerequisites on 1.1
        for (unsigned k = 0; k < sizeof need / sizeof *need; k++) { if (!has_ext(de, nde, need[k])) VDIE("Vulkan %u.%u device without %s", VK_API_VERSION_MAJOR(g_api), VK_API_VERSION_MINOR(g_api), need[k]); dext[ndext++] = need[k]; }
        if (old11) for (unsigned k = 0; k < sizeof deps11 / sizeof *deps11; k++) if (has_ext(de, nde, deps11[k])) dext[ndext++] = deps11[k];
        VLOG("Vulkan %u.%u device: dynamic rendering, timeline semaphores and extended dynamic state through their extensions", VK_API_VERSION_MAJOR(g_api), VK_API_VERSION_MINOR(g_api));
    }
    if (has_ext(de, nde, "VK_KHR_push_descriptor") && !getenv("DM_VK_NO_PUSH")) dext[ndext++] = "VK_KHR_push_descriptor";
    else { g_nopush = 1; VLOG("no push descriptors: descriptor sets from per-submission pools"); }
    if (has_ext(de, nde, "VK_KHR_portability_subset")) { dext[ndext++] = "VK_KHR_portability_subset"; g_moltenvk = 1; }
    if (!g_nopush) { VkPhysicalDevicePushDescriptorPropertiesKHR pd = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd }; vkGetPhysicalDeviceProperties2(g_pd, &pp);
        if (pd.maxPushDescriptors) g_maxpush = pd.maxPushDescriptors; if (getenv("DM_VK_MAXPUSH")) g_maxpush = (uint32_t)atoi(getenv("DM_VK_MAXPUSH"));   // a lower limit tests the set path
        VLOG("push descriptors: up to %u per program", g_maxpush); }
    if ((g_host_import = has_ext(de, nde, "VK_EXT_external_memory_host"))) dext[ndext++] = "VK_EXT_external_memory_host";
    if ((g_has_swapchain = has_ext(de, nde, "VK_KHR_swapchain"))) dext[ndext++] = "VK_KHR_swapchain";
    if ((g_clipctl = has_ext(de, nde, "VK_EXT_depth_clip_control"))) dext[ndext++] = "VK_EXT_depth_clip_control";
    if ((g_border = has_ext(de, nde, "VK_EXT_custom_border_color"))) dext[ndext++] = "VK_EXT_custom_border_color";
    if ((g_divisor = has_ext(de, nde, "VK_EXT_vertex_attribute_divisor"))) dext[ndext++] = "VK_EXT_vertex_attribute_divisor";
    if ((g_eds2 = has_ext(de, nde, "VK_EXT_extended_dynamic_state2"))) dext[ndext++] = "VK_EXT_extended_dynamic_state2";
    int compctl = has_ext(de, nde, "VK_EXT_image_compression_control"); if (compctl) dext[ndext++] = "VK_EXT_image_compression_control";   // to read whether render targets stay compressed
#ifdef __linux__
    if (!g_host_import && has_ext(de, nde, "VK_KHR_external_memory_fd") && !(getenv("DM_VK_NO_FDMAP") && atoi(getenv("DM_VK_NO_FDMAP")))) { dext[ndext++] = "VK_KHR_external_memory_fd"; g_fdmap = -1; }
    VLOG("memory pools: fd export %s (host import %d, VK_KHR_external_memory_fd %d)", g_fdmap ? "to try" : "off", g_host_import, has_ext(de, nde, "VK_KHR_external_memory_fd"));
#endif
    free(de);
    if (getenv("DM_VK_NO_HOST_IMPORT")) g_host_import = 0;
    // Every feature the device has is enabled (the translated shaders declare what they use: 64-bit integers, image formats without a type, …), robust buffer access included:
    // uniform blocks are declared 64 KB and bound with the game's smaller ranges, and Maxwell reads 0 past them
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fbc = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
    VkPhysicalDeviceDepthClipControlFeaturesEXT fcc = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, .pNext = g_border ? &fbc : NULL };
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = g_clipctl ? (void *)&fcc : g_border ? (void *)&fbc : NULL };
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT fe2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .pNext = g_divisor ? (void *)&fdv : g_clipctl ? (void *)&fcc : g_border ? (void *)&fbc : NULL };
    void *opt = g_eds2 ? (void *)&fe2 : g_divisor ? (void *)&fdv : g_clipctl ? (void *)&fcc : g_border ? (void *)&fbc : NULL;
    VkPhysicalDeviceImageCompressionControlFeaturesEXT fic = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT, .pNext = opt };
    if (compctl) opt = &fic;
    // Vulkan 1.3: its own feature blocks. Older devices: the blocks of their version plus the extensions' own structures (the 1.2 / 1.3 blocks are not allowed there)
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT fed = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .pNext = opt };
    VkPhysicalDeviceTimelineSemaphoreFeatures fts = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = &fed };
    VkPhysicalDeviceDynamicRenderingFeatures fdr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .pNext = old11 ? (void *)&fts : (void *)&fed };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = opt };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = old ? (void *)&fdr : (void *)&f13 };
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = old11 ? (void *)&fdr : (void *)&f11 };
    vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    int dynren = old ? fdr.dynamicRendering : f13.dynamicRendering, timeline = old11 ? fts.timelineSemaphore : f12.timelineSemaphore;
    if (!dynren || !timeline || (old && !fed.extendedDynamicState)) VDIE("dynamic rendering %d / timeline semaphores %d / extended dynamic state %d missing", dynren, timeline, old ? fed.extendedDynamicState : 1);
    if (!fcc.depthClipControl) g_clipctl = 0;
    if (!fbc.customBorderColors) g_border = 0;
    // BC1–BC7: sampled directly where the GPU has them (Adreno, Xclipse, desktop), else decoded on upload (Mali, PowerVR; DM_VK_NO_BC=1 tries that path anywhere)
    if (getenv("DM_VK_NO_BC") && atoi(getenv("DM_VK_NO_BC"))) f2.features.textureCompressionBC = VK_FALSE;
    nvn_fmt_vk_bc(f2.features.textureCompressionBC);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext };
    VKCHECK(vkCreateDevice(g_pd, &dci, NULL, &g_dev));
    VK_DEVICE(VK_LOAD_D)
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    p_push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSetKHR");
    if (compctl && fic.imageCompressionControl) p_layout2 = (PFN_vkGetImageSubresourceLayout2EXT)vkGetDeviceProcAddr(g_dev, "vkGetImageSubresourceLayout2EXT");
    p_hostprops = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_dev, "vkGetMemoryHostPointerPropertiesEXT");
    if (g_host_import) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp }; vkGetPhysicalDeviceProperties2(g_pd, &pp);
        g_host_align = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    }
    {   // 24-bit depth where the device renders to it (Adreno), else the 32-bit float formats (Apple GPUs)
        VkFormatProperties a, b; vkGetPhysicalDeviceFormatProperties(g_pd, VK_FORMAT_X8_D24_UNORM_PACK32, &a); vkGetPhysicalDeviceFormatProperties(g_pd, VK_FORMAT_D24_UNORM_S8_UINT, &b);
        nvn_fmt_vk_depth24(!!(a.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT), !!(b.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
    }
    VLOG("Vulkan device %s (API %u.%u.%u, driver %#x): pools %s, depth clip control %d, custom border colours %d, vertex divisor %d, extended dynamic state 2 %d, BC textures %s",
         g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion), VK_API_VERSION_MINOR(g_props.apiVersion), VK_API_VERSION_PATCH(g_props.apiVersion), g_props.driverVersion,
         g_host_import ? "imported host memory" : "driver memory moved onto the pool (mremap)", g_clipctl, g_border, g_divisor, g_eds2,
         nvn_fmt_vk_decode(0x46) ? "decoded on upload" : "sampled by the GPU");
    completion_start();
    dummies_init();
    pcache_open();
    if (getenv("DM_VK_PSO_SEED") || getenv("DM_VK_PSO_RECORD")) { pthread_t t; if (!pthread_create(&t, NULL, prewarm_main, NULL)) pthread_detach(t); }
}

// ================= Device memory (images, internal buffers) =================
// A first-fit allocator over 64 MB blocks per memory type (larger requests get a block of their own): drivers cap the number of allocations (4096 is common), and a game holds
// thousands of textures
typedef struct { VkDeviceMemory mem; VkDeviceSize size; uint32_t type; void *map; int nfree, cfree; struct { VkDeviceSize off, size; } *fr; int used; } vblock;
typedef struct { vblock *b; VkDeviceSize off, size; } vmalloc_t;
static vblock **g_blocks; static int g_nblocks; static pthread_mutex_t g_amu = PTHREAD_MUTEX_INITIALIZER;
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_memp.memoryTypes[i].propertyFlags & want) == want && !(g_memp.memoryTypes[i].propertyFlags & avoid)) return (int)i;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_memp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static void fr_add(vblock *b, VkDeviceSize off, VkDeviceSize size) {   // insert a free range keeping order, merging with its neighbours
    int i = 0; while (i < b->nfree && b->fr[i].off < off) i++;
    if (i > 0 && b->fr[i - 1].off + b->fr[i - 1].size == off) { b->fr[i - 1].size += size; if (i < b->nfree && off + size == b->fr[i].off) { b->fr[i - 1].size += b->fr[i].size; memmove(b->fr + i, b->fr + i + 1, (b->nfree - i - 1) * sizeof *b->fr); b->nfree--; } return; }
    if (i < b->nfree && off + size == b->fr[i].off) { b->fr[i].off = off; b->fr[i].size += size; return; }
    if (b->nfree == b->cfree) { b->cfree = b->cfree ? b->cfree * 2 : 16; b->fr = realloc(b->fr, b->cfree * sizeof *b->fr); }
    memmove(b->fr + i + 1, b->fr + i, (b->nfree - i) * sizeof *b->fr); b->fr[i].off = off; b->fr[i].size = size; b->nfree++;
}
static vmalloc_t vk_alloc(VkMemoryRequirements rq, VkMemoryPropertyFlags want, int map) {
    int t = mem_type(rq.memoryTypeBits, want, map ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (t < 0) VDIE("no memory type for bits %#x flags %#x", rq.memoryTypeBits, want);
    VkDeviceSize al = rq.alignment > g_props.limits.bufferImageGranularity ? rq.alignment : g_props.limits.bufferImageGranularity;
    pthread_mutex_lock(&g_amu);
    for (int k = 0; k < g_nblocks; k++) {
        vblock *b = g_blocks[k]; if (b->type != (uint32_t)t || (map && !b->map)) continue;
        for (int i = 0; i < b->nfree; i++) {
            VkDeviceSize o = (b->fr[i].off + al - 1) / al * al, end = b->fr[i].off + b->fr[i].size;
            if (o + rq.size > end) continue;
            VkDeviceSize f0 = b->fr[i].off, f1 = end; memmove(b->fr + i, b->fr + i + 1, (b->nfree - i - 1) * sizeof *b->fr); b->nfree--;
            if (o > f0) fr_add(b, f0, o - f0);
            if (o + rq.size < f1) fr_add(b, o + rq.size, f1 - o - rq.size);
            b->used++; pthread_mutex_unlock(&g_amu);
            return (vmalloc_t){b, o, rq.size};
        }
    }
    VkDeviceSize bs = rq.size > (32ull << 20) ? (rq.size + 0xffff) & ~0xffffull : 64ull << 20;
    { static long cap = -2; if (cap == -2) { const char *e = getenv("DM_VK_MEM_CAP_MB"); cap = e ? atol(e) : -1; }   // test runs: stop before the machine runs out of memory
      double total = bs; for (int k = 0; k < g_nblocks; k++) total += g_blocks[k]->size;
      if (cap > 0 && total > cap * 1048576.0) { pthread_mutex_unlock(&g_amu); VDIE("device memory would pass DM_VK_MEM_CAP_MB=%ld (%.0f MB in %d blocks)", cap, total / 1048576, g_nblocks); } }
    vblock *b = calloc(1, sizeof *b); b->size = bs; b->type = (uint32_t)t;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bs, .memoryTypeIndex = (uint32_t)t };
    VkResult r = vkAllocateMemory(g_dev, &mai, NULL, &b->mem);
    if (r) { pthread_mutex_unlock(&g_amu); VDIE("vkAllocateMemory %llu bytes type %d failed: %d", (unsigned long long)bs, t, r); }
    if (g_memp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) vkMapMemory(g_dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map);
    if (rq.size < bs) fr_add(b, rq.size, bs - rq.size);
    b->used = 1;
    g_blocks = realloc(g_blocks, (g_nblocks + 1) * sizeof *g_blocks); g_blocks[g_nblocks++] = b;
    pthread_mutex_unlock(&g_amu);
    if (getenv("DM_VK_BLOCKLOG")) VLOG("device block %d: %.0f MB type %d for a %.1f MB request from %p", g_nblocks, bs / 1048576.0, t, rq.size / 1048576.0, __builtin_return_address(0));
    return (vmalloc_t){b, 0, rq.size};
}
static void vk_free(vmalloc_t a) {
    if (!a.b) return;
    pthread_mutex_lock(&g_amu); fr_add(a.b, a.off, a.size); a.b->used--; pthread_mutex_unlock(&g_amu);
}

// ================= Deferred destruction: objects the GPU may still use go when the submissions recorded so far are done =================
enum { DF_IMAGE, DF_VIEW, DF_BUFFER, DF_BUFVIEW, DF_ALLOC, DF_PIPELINE };
typedef struct { int kind; uint64_t fence; union { VkImage img; VkImageView view; VkBuffer buf; VkBufferView bview; VkPipeline pipe; } h; vmalloc_t a; } dfr;
static dfr *g_dfr; static int g_ndfr, g_cdfr; static pthread_mutex_t g_dfmu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;                  // last fence value handed to a submission (submit thread)
static _Atomic uint64_t g_done;           // last fence value the GPU completed (completion thread)
static void defer_add(dfr d) {
    d.fence = g_fence + 1;
    pthread_mutex_lock(&g_dfmu);
    if (g_ndfr == g_cdfr) { g_cdfr = g_cdfr ? g_cdfr * 2 : 256; g_dfr = realloc(g_dfr, g_cdfr * sizeof *g_dfr); }
    g_dfr[g_ndfr++] = d; pthread_mutex_unlock(&g_dfmu);
}
static void defer_drain(void) {
    uint64_t done = atomic_load(&g_done);
    pthread_mutex_lock(&g_dfmu);
    for (int i = 0; i < g_ndfr;) {
        dfr d = g_dfr[i];
        if (d.fence > done) { i++; continue; }
        g_dfr[i] = g_dfr[--g_ndfr];
        switch (d.kind) {
            case DF_IMAGE: vkDestroyImage(g_dev, d.h.img, NULL); break; case DF_VIEW: vkDestroyImageView(g_dev, d.h.view, NULL); break;
            case DF_BUFFER: vkDestroyBuffer(g_dev, d.h.buf, NULL); break; case DF_BUFVIEW: vkDestroyBufferView(g_dev, d.h.bview, NULL); break;
            case DF_PIPELINE: vkDestroyPipeline(g_dev, d.h.pipe, NULL); break; case DF_ALLOC: break;
        }
        vk_free(d.a);
    }
    pthread_mutex_unlock(&g_dfmu);
}

// ================= Memory pools: GPU buffers over the game's pages =================
// Three ways a pool's pages and the GPU share memory: imported host memory (VK_EXT_external_memory_host), the driver's memory moved onto the pool (mremap, needs every
// allocation to be a whole mapping of its own: Adreno), or a mirror (shadow): the GPU buffer in the driver's memory, the game's pages unchanged, the pages the game wrote
// copied in before each submission and what the GPU wrote copied back once it is done (drivers that sub-allocate their mappings: the Android emulator's; DM_VK_POOL_SHADOW=1)
typedef struct { uintptr_t lo, hi, base; VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; const void *key; void *moved_from; uint8_t *shadow; uint64_t stamp; int tracked_here, fdmapped; } pool_ent;
static pool_ent *g_pools; static int g_npool, g_cpool; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_nshadow;   // mirrored pools alive
static int g_plast = -1;
static const VkBufferUsageFlags BUF_ALL = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
    | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
// Pool memory of the mremap route is never given back to the driver: Adreno hands the mappings of freed allocations out again, and one whose pages had been moved
// crashed the next pool (loading a save, leaving the house in Shield) or lost the device. A freed pool's buffer and memory, its pages moved back where the driver
// mapped them, wait here for the next pool of the same size
#ifdef __linux__
typedef struct { VkDeviceSize n; VkBuffer buf; VkDeviceMemory mem; void *p; } pool_mem;
static pool_mem g_pmem[1024]; static int g_npmem; static pthread_mutex_t g_pmmu = PTHREAD_MUTEX_INITIALIZER;
static int pool_mem_take(VkDeviceSize n, VkBuffer *buf, VkDeviceMemory *m, void **p) {
    pthread_mutex_lock(&g_pmmu); int ok = 0;
    for (int i = g_npmem - 1; i >= 0; i--) if (g_pmem[i].n == n) { *buf = g_pmem[i].buf; *m = g_pmem[i].mem; *p = g_pmem[i].p; g_pmem[i] = g_pmem[--g_npmem]; ok = 1; break; }
    pthread_mutex_unlock(&g_pmmu); return ok;
}
static int pool_mem_put(VkDeviceSize n, VkBuffer buf, VkDeviceMemory m, void *p) {
    pthread_mutex_lock(&g_pmmu); int ok = g_npmem < (int)(sizeof g_pmem / sizeof *g_pmem);
    if (ok) g_pmem[g_npmem++] = (pool_mem){ n, buf, m, p };
    pthread_mutex_unlock(&g_pmmu); return ok;
}
#endif
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init();
    uintptr_t pg = (uintptr_t)(g_host_import ? g_host_align : (VkDeviceSize)getpagesize()), lo = (uintptr_t)mem, hi = lo + size;
    uintptr_t a = lo & ~(pg - 1), b = (hi + pg - 1) & ~(pg - 1); VkDeviceSize n = b - a;
    pool_ent e = { lo, hi, a, VK_NULL_HANDLE, VK_NULL_HANDLE, n, mem, NULL, NULL, 0, 0 };
    void *p = NULL; int reused = 0;
#ifdef __linux__
    static int shadow_forced = -1; if (shadow_forced < 0) shadow_forced = getenv("DM_VK_POOL_SHADOW") && atoi(getenv("DM_VK_POOL_SHADOW"));
    if (!g_host_import && !shadow_forced && !g_fdmap && pool_mem_take(n, &e.buf, &e.mem, &p)) { reused = 1; goto reuse; }
#endif
    VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkExternalMemoryBufferCreateInfo ebfd = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = g_host_import ? &eb : g_fdmap ? &ebfd : NULL, .size = n, .usage = BUF_ALL };
    VkResult br = vkCreateBuffer(g_dev, &bi, NULL, &e.buf);
#ifdef __linux__
    if (br == VK_ERROR_INVALID_EXTERNAL_HANDLE && g_fdmap < 0) {
        VLOG("[RED] opaque-fd external buffer rejected (%d): moving driver mappings instead (mremap)", br);
        g_fdmap = 0;
        bi.pNext = NULL;
        br = vkCreateBuffer(g_dev, &bi, NULL, &e.buf);
    }
#endif
    if (br != VK_SUCCESS) VDIE("vkCreateBuffer(g_dev, &bi, NULL, &e.buf) failed: %d (%s:%d)", br, __FILE__, __LINE__);
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, e.buf, &rq);
    if (g_host_import) {
        VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        p_hostprops(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)a, &hp);
        int t = mem_type(rq.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
        VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)a };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = n, .memoryTypeIndex = (uint32_t)(t < 0 ? 0 : t) };
        VkResult r = t < 0 ? VK_ERROR_OUT_OF_HOST_MEMORY : vkAllocateMemory(g_dev, &mai, NULL, &e.mem);
        if (r) {   // a pool the import refuses (Pokémon Legends: Arceus on MoltenVK, a 344 KB pool, -2 about 25 s into loading a save) is mirrored instead, as on drivers without whole mappings
            RED_ONCE(60, "memory pool %p+%#llx: host memory import failed (%d): mirrored", mem, (unsigned long long)size, r);
            vkDestroyBuffer(g_dev, e.buf, NULL); bi.pNext = NULL; VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &e.buf)); vkGetBufferMemoryRequirements(g_dev, e.buf, &rq);
            int ts = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
            VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size > n ? rq.size : n, .memoryTypeIndex = (uint32_t)ts };
            VKCHECK(vkAllocateMemory(g_dev, &ma, NULL, &e.mem));
            void *p = NULL; VKCHECK(vkMapMemory(g_dev, e.mem, 0, VK_WHOLE_SIZE, 0, &p));
            e.shadow = p; atomic_fetch_add(&g_nshadow, 1);
            if (!nvn_dirty_tracked((void *)a, n)) { nvn_dirty_add_range((void *)a, n); e.tracked_here = 1; }
            e.stamp = nvn_dirty_now(); nvn_dirty_protect((void *)a, n);
            memcpy(p, (void *)a, n);
        }
    } else {
        // The driver's own memory, preferably cached and coherent (Adreno type 0xf), filled with what the pool holds, then moved onto the pool's pages: from here on the game's
        // pointers and the GPU address the same memory. The driver's old mapping range gets a PROT_NONE placeholder, so nothing else lands where the driver believes its mapping is
        int t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
        if (t < 0) t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        // a dedicated allocation: a mapping of its own, never a piece of a bigger block the driver sub-allocates from (Adreno did that for small pools, 8 and 16 KB, and
        // handed out addresses inside blocks whose pages an earlier pool had moved away: its write to the placeholder crashed, loading a save and changing areas in Shield)
        VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = e.buf };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = rq.size > n ? rq.size : n, .memoryTypeIndex = (uint32_t)t };
#ifdef __linux__
        // The fd route: the driver's own mapping is never touched (moving it broke Adreno: freed mappings handed out again, crashes and a lost device on area changes);
        // the same pages are mapped once more, shared, over the pool, so the game's pointers and the GPU still address the same memory without copies
        if (g_fdmap) {
            static PFN_vkGetMemoryFdKHR getfd; if (!getfd) getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_dev, "vkGetMemoryFdKHR");
            VkExportMemoryAllocateInfo ex = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
            VkMemoryAllocateInfo xai = mai; xai.pNext = &ex;
            int fd = -1; void *tmp = MAP_FAILED;
            if (getfd && !vkAllocateMemory(g_dev, &xai, NULL, &e.mem)) {
                VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = e.mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
                if (!getfd(g_dev, &gi, &fd) && fd >= 0) tmp = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (tmp != MAP_FAILED) {
                    memcpy(tmp, (void *)a, n);   // what the pool holds, into the GPU memory
                    void *q = mmap((void *)a, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);   // the pool's pages become the GPU memory, in one step
                    munmap(tmp, n);
                    if (q == (void *)a) {
                        close(fd); e.fdmapped = 1;
                        if (g_fdmap < 0) { g_fdmap = 1; VLOG("memory pools: the driver's memory exported (fd) and mapped over the pool's pages"); }
                        goto bind;
                    }
                    if (g_fdmap > 0) VDIE("memory pool %p+%#llx: mapping the GPU memory over it failed (errno %d)", mem, (unsigned long long)size, errno);
                }
                if (fd >= 0) close(fd);
                vkFreeMemory(g_dev, e.mem, NULL); e.mem = VK_NULL_HANDLE;
            }
            if (g_fdmap > 0) VDIE("memory pool %p+%#llx: GPU memory export failed", mem, (unsigned long long)size);
            VLOG("memory pools: the driver's memory cannot be exported and mapped (errno %d): moving its mappings instead (mremap)", errno);
            g_fdmap = 0;   // first pool: fall back for good (the buffer was made for export, which the plain allocation below also accepts)
        }
#endif
#ifdef __linux__
        // The mapping the driver returns must be readable and writable over the whole pool. Adreno hands out the CPU mappings of freed allocations again, and one whose
        // pages a pool had moved away (a placeholder, or a range of the driver's that is gone) crashed the first write here (loading a save, changing areas in Shield).
        // Such an allocation is kept aside (never freed, so it is never handed out again) and another one made
        uintptr_t vs = 0, ve = 0;
        for (int tries = 0;; tries++) {
            VKCHECK(vkAllocateMemory(g_dev, &mai, NULL, &e.mem));
            VKCHECK(vkMapMemory(g_dev, e.mem, 0, VK_WHOLE_SIZE, 0, &p));
            int rw = 0; vs = ve = 0; FILE *mf = fopen("/proc/self/maps", "r"); char ln[512];
            while (mf && fgets(ln, sizeof ln, mf)) { unsigned long s0, e0; char pr[8] = "";
                if (sscanf(ln, "%lx-%lx %7s", &s0, &e0, pr) == 3 && (uintptr_t)p >= s0 && (uintptr_t)p < e0) { vs = s0; ve = e0; rw = pr[0] == 'r' && pr[1] == 'w'; break; } }
            if (mf) fclose(mf);
            if (rw && ve >= (uintptr_t)p + n) break;
            VLOG("[RED] memory pool %p+%#llx: the driver's mapping %p is not usable (%s): kept aside, allocating again", mem, (unsigned long long)size, p, rw ? "too short" : "not writable");
            if (tries >= 16) VDIE("memory pool %p+%#llx: no usable GPU memory mapping after %d tries", mem, (unsigned long long)size, tries + 1);
        }
        {   // mremap moves whole mappings: the driver's must start at p, be page-aligned, and end exactly after the pool
            static int force = -1; if (force < 0) force = getenv("DM_VK_POOL_SHADOW") && atoi(getenv("DM_VK_POOL_SHADOW"));
            if (force || ((uintptr_t)p & (getpagesize() - 1)) || vs != (uintptr_t)p || ve != (uintptr_t)p + n) {
                static int said; if (!said++) VLOG("memory pools: %s: mirrored (pages the game writes copied in before each submission)", force ? "DM_VK_POOL_SHADOW" : "the driver's mappings are not whole mappings of their own");
                e.shadow = p; atomic_fetch_add(&g_nshadow, 1);
                if (!nvn_dirty_tracked((void *)a, n)) { nvn_dirty_add_range((void *)a, n); e.tracked_here = 1; }
                e.stamp = nvn_dirty_now(); nvn_dirty_protect((void *)a, n);   // later writes fault and get a newer generation; then the copy
                memcpy(p, (void *)a, n);
                goto bind;
            }
        }
#else
        VKCHECK(vkAllocateMemory(g_dev, &mai, NULL, &e.mem)); VKCHECK(vkMapMemory(g_dev, e.mem, 0, VK_WHOLE_SIZE, 0, &p));
#endif
#ifdef __linux__
reuse:
#endif
        memcpy(p, (void *)a, n);
#ifdef __linux__
        // MREMAP_DONTUNMAP leaves the driver's old range mapped (empty) in the same step, so no other thread's mapping can land in a gap there before the placeholder;
        // where the kernel refuses it for this mapping, the plain move and the placeholder right after
#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif
        void *q = mremap(p, n, n, MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, (void *)a);
        if (q == (void *)a) mmap(p, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);   // the empty old range becomes the placeholder (replaced in one step)
        else {
            static int said; if (!said++) VLOG("pool memory: MREMAP_DONTUNMAP refused (errno %d), moving without it", errno);
            q = mremap(p, n, n, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)a);
            if (q != (void *)a) {
                int err = errno;
                VLOG("[RED] memory pool %p+%#llx: moving the GPU memory onto it failed (errno %d): mirrored instead", mem, (unsigned long long)size, err);
                e.shadow = p; atomic_fetch_add(&g_nshadow, 1);
                if (!nvn_dirty_tracked((void *)a, n)) { nvn_dirty_add_range((void *)a, n); e.tracked_here = 1; }
                e.stamp = nvn_dirty_now(); nvn_dirty_protect((void *)a, n);
                memcpy(p, (void *)a, n);
                goto bind;
            }
            mmap(p, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        }
        e.moved_from = p;
#else
        VDIE("memory pools need VK_EXT_external_memory_host on this platform");
#endif
    }
#ifdef __linux__
bind:
#endif
    if (!reused) VKCHECK(vkBindBufferMemory(g_dev, e.buf, e.mem, 0));
    pthread_mutex_lock(&g_mu);
    if (g_npool == g_cpool) { g_cpool = g_cpool ? g_cpool * 2 : 64; g_pools = realloc(g_pools, g_cpool * sizeof *g_pools); }
    g_pools[g_npool++] = e; g_plast = -1;
    pthread_mutex_unlock(&g_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].key == mem) {
        pool_ent e = g_pools[i]; g_pools[i] = g_pools[--g_npool]; g_plast = -1;
        pthread_mutex_unlock(&g_mu);
        // The game gets its pages back as ordinary memory with the same content (the driver's pages there replaced in one step, never leaving a gap another thread's
        // mapping could take); the GPU buffer and its memory go once the GPU is done with them. The placeholder at the driver's old address is the driver's to unmap
        // (vkFreeMemory): unmapping that range here as well, after the free, raced other threads whose new mappings could already sit there (games free and make pools
        // all the time: 131 times in two minutes of Pokémon Shield's character creation)
        mtl_finish();
        if (e.shadow) { atomic_fetch_sub(&g_nshadow, 1); if (e.tracked_here) nvn_dirty_remove_range((void *)e.base); }
        if (e.fdmapped) {   // the game's pages back as ordinary memory with the same content, then a plain free (the driver's own mapping was never touched)
            void *keep = malloc(e.size); memcpy(keep, (void *)e.base, e.size);
            mmap((void *)e.base, e.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            memcpy((void *)e.base, keep, e.size); free(keep);
            vkDestroyBuffer(g_dev, e.buf, NULL); vkFreeMemory(g_dev, e.mem, NULL);
            return;
        }
        int keep_mem = 0;   // the allocation stays when its pages could not go back (the driver would hand out the placeholder again)
        if (e.moved_from) {
            // The driver's pages go back where the driver mapped them (over the placeholder) before the free: Adreno keeps freed allocations with their mappings and hands
            // them out again, so a later pool's vkMapMemory returned the placeholder's address and the first write there crashed (loading a save in Pokémon Shield)
            void *keep = malloc(e.size); memcpy(keep, (void *)e.base, e.size);
#ifdef __linux__
            // one protection over the whole range first: the texture tracking's write protection splits it into several mappings, and mremap moves one (EFAULT otherwise)
            mprotect((void *)e.base, e.size, PROT_READ | PROT_WRITE);
            void *q = mremap((void *)e.base, e.size, e.size, MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, e.moved_from);
            if (q != e.moved_from) q = mremap((void *)e.base, e.size, e.size, MREMAP_MAYMOVE | MREMAP_FIXED, e.moved_from);   // (then the gap below is filled at once)
            if (q != e.moved_from) { VLOG("[RED] memory pool %p: the GPU memory could not go back to the driver (errno %d): kept, not freed", (void *)e.base, errno); keep_mem = 1; }
            else mprotect(e.moved_from, e.size, PROT_READ | PROT_WRITE);   // pages the texture tracking had write-protected come back writable
#endif
            mmap((void *)e.base, e.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            memcpy((void *)e.base, keep, e.size); free(keep);
        }
#ifdef __linux__
        if (e.moved_from && !keep_mem) { if (!pool_mem_put(e.size, e.buf, e.mem, e.moved_from)) vkDestroyBuffer(g_dev, e.buf, NULL); return; }   // kept for the next pool (full: kept aside)
        if (e.moved_from) { vkDestroyBuffer(g_dev, e.buf, NULL); return; }   // pages not back with the driver: never freed
#endif
        vkDestroyBuffer(g_dev, e.buf, NULL); if (!keep_mem) vkFreeMemory(g_dev, e.mem, NULL);
        return;
    }
    pthread_mutex_unlock(&g_mu);
}
static int pool_find(uint64_t addr, uint64_t len) {   // caller holds g_mu
    if (g_plast >= 0 && g_plast < g_npool && addr >= g_pools[g_plast].base && addr + len <= g_pools[g_plast].base + g_pools[g_plast].size) return g_plast;
    for (int i = 0; i < g_npool; i++) if (addr >= g_pools[i].base && addr + len <= g_pools[i].base + g_pools[i].size) return g_plast = i;
    return -1;
}
// GPU buffer + offset of [addr, addr + len); 0 when it is in no pool
static int pool_buf(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *left) {
    pthread_mutex_lock(&g_mu); int i = pool_find(addr, len ? len : 1);
    if (i >= 0) { *buf = g_pools[i].buf; *off = addr - g_pools[i].base; if (left) *left = g_pools[i].base + g_pools[i].size - addr; }
    pthread_mutex_unlock(&g_mu);
    return i >= 0;
}
// Mirrored pools: the pages the game wrote since the last submission go into the GPU's copy (called before every submission)
static void shadow_copy_run(uintptr_t a, size_t n, void *u) { pool_ent *e = u; nvn_dirty_protect((void *)a, n); memcpy(e->shadow + (a - e->base), (void *)a, n); }
static void shadow_sync(void) {
    if (!atomic_load(&g_nshadow)) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++) { pool_ent *e = &g_pools[i]; if (!e->shadow) continue;
        uint64_t s0 = nvn_dirty_now(); nvn_dirty_runs((void *)e->base, e->size, e->stamp, shadow_copy_run, e); e->stamp = s0; }
    pthread_mutex_unlock(&g_mu);
}
// What the GPU writes into a mirrored pool (copies, fills, readbacks, writable storage buffers) is copied back to the game's pages when the submission is done
typedef struct { uint64_t addr, n; } wb_t;
static void wb_note(uint64_t addr, uint64_t n);
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b; VkDeviceSize o; if (!pool_buf(addr, len, &b, &o, NULL)) return 0;
    *buf = (void *)b; *off = o; return 1;
}
void mtl_alias_drop_pool(const void *pool);   // textures (section below)

// ================= Submissions =================
// A submission (mtl_sub_begin … mtl_sub_commit) records into its main command buffer; texture uploads and the first-use setup of new images go into its upload command buffer,
// submitted just before it. CPU-visible effects (event writes, counter reports) are collected and applied by the completion thread when the GPU has finished the submission,
// in submission order, before the fence value is published (games wait on either)
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } post_op;
typedef struct { VkBuffer buf; vmalloc_t a; uint8_t *map; VkDeviceSize used, size; } arena_blk;
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb, ucb; int ucb_open; uint64_t fence;
    post_op *post; int npost, cpost;
    arena_blk *ar; int nar, car;   // this submission's arena blocks (host-visible): driver segments, converted indices, upload staging
    wb_t *wb; int nwb, cwb;        // mirrored pools: ranges the GPU writes in this submission (copied back to the game's pages at completion)
    VkDescriptorPool *dp; int ndp, cdp;   // g_nopush: this submission's descriptor pools (dp[cdp] in use), reset when it is reused
    uint64_t frame;                 // the frame it belongs to (GPU time per frame, mtl_gpu_ms)
} sub_t;
#define NSUB 6
static sub_t g_subs[NSUB]; static sub_t *g_sub; static int g_sub_next;
static void wb_note(uint64_t addr, uint64_t n) {
    if (!n || !g_sub || !atomic_load(&g_nshadow)) return;
    pthread_mutex_lock(&g_mu); int k = pool_find(addr, n); int sh = k >= 0 && g_pools[k].shadow; pthread_mutex_unlock(&g_mu);
    if (!sh) return;
    sub_t *s = g_sub; if (s->nwb == s->cwb) { s->cwb = s->cwb ? s->cwb * 2 : 64; s->wb = realloc(s->wb, sizeof *s->wb * (size_t)s->cwb); }
    s->wb[s->nwb++] = (wb_t){ addr, n };
}
static VkSemaphore g_tl;
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;
static sub_t *g_inflight[NSUB * 2]; static int g_nin;   // submitted, not yet completed, in submission order (under g_cmu)
static pthread_t g_sub_thread; static _Atomic int g_sub_thread_set;
static int on_sub_thread(void) { return atomic_load(&g_sub_thread_set) && pthread_equal(pthread_self(), g_sub_thread); }

static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) g = mono_ns(); return g * 12 / 625; }
static void wb_apply(sub_t *s) {
    for (int i = 0; i < s->nwb; i++) {
        pthread_mutex_lock(&g_mu); int k = pool_find(s->wb[i].addr, s->wb[i].n); pool_ent e = k >= 0 ? g_pools[k] : (pool_ent){0}; pthread_mutex_unlock(&g_mu);
        if (k < 0 || !e.shadow) continue;
        dm_guest_write_prepare((void *)(uintptr_t)s->wb[i].addr, s->wb[i].n);
        memcpy((void *)(uintptr_t)s->wb[i].addr, e.shadow + (s->wb[i].addr - e.base), s->wb[i].n);
    }
    s->nwb = 0;
}
static void post_apply(sub_t *s) {
    wb_apply(s);
    for (int i = 0; i < s->npost; i++) {
        post_op *o = &s->post[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; }
        else {   // counter report {value, timestamp}: timestamps on the guest's 19.2 MHz clock; occlusion counts are not measured yet (reported as visible)
            uint64_t t = gpu_ticks(), rec[2] = { o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 0xffffu : (o->type == 0 || o->type == 0x10) && !DM_GAME_RENDER_COND ? t : 0, t };
            memcpy((void *)o->addr, rec, sizeof rec);
        }
    }
    s->npost = 0;
}
// GPU time per frame: a timestamp at the start and the end of every submission's main command buffer, summed per frame once it completed (mtl_gpu_ms; the
// frame report's GPU column and the long-frame lines)
static VkQueryPool g_tsq; static double g_tsper;
enum { GRING = 64 };
static struct { _Atomic uint64_t frame, ns; _Atomic int pending; } g_gpu[GRING];
static void gpu_ts_done(sub_t *s) {
    if (!g_tsq) return;
    uint64_t t[2]; int i = (int)(s - g_subs);
    if (vkGetQueryPoolResults(g_dev, g_tsq, (uint32_t)i * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) atomic_fetch_add(&g_gpu[s->frame % GRING].ns, (uint64_t)((t[1] - t[0]) * g_tsper));
    atomic_fetch_sub(&g_gpu[s->frame % GRING].pending, 1);
}
static void *completion_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_cmu);
        while (!g_nin) pthread_cond_wait(&g_ccv, &g_cmu);
        sub_t *s = g_inflight[0]; uint64_t want = s->fence;
        pthread_mutex_unlock(&g_cmu);
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_tl, .pValues = &want };
        VkResult r = vkWaitSemaphores(g_dev, &wi, 2000000000ull);
        if (r == VK_TIMEOUT) continue;
        if (r) VDIE("vkWaitSemaphores failed: %d (device lost?)", r);
        post_apply(s); gpu_ts_done(s);
        pthread_mutex_lock(&g_cmu);
        memmove(g_inflight, g_inflight + 1, (size_t)--g_nin * sizeof *g_inflight);
        atomic_store(&g_done, want);
        pthread_cond_broadcast(&g_ccv);
        pthread_mutex_unlock(&g_cmu);
    }
    return NULL;
}
static void completion_start(void) {
    VkSemaphoreTypeCreateInfo st = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st };
    VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_tl));
    for (int i = 0; i < NSUB; i++) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = g_qf };
        VKCHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_subs[i].pool));
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_subs[i].pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VKCHECK(vkAllocateCommandBuffers(g_dev, &cai, &g_subs[i].cb)); VKCHECK(vkAllocateCommandBuffers(g_dev, &cai, &g_subs[i].ucb));
    }
    {   uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
        if (g_qf < nq && qp[g_qf].timestampValidBits && g_props.limits.timestampPeriod > 0 && getenv("DM_VK_GPU_TIME") && atoi(getenv("DM_VK_GPU_TIME"))) {   // on request only (DM_VK_GPU_TIME=1): timestamps may cost on tilers
            VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * NSUB };
            if (vkCreateQueryPool(g_dev, &qi, NULL, &g_tsq)) g_tsq = VK_NULL_HANDLE; g_tsper = g_props.limits.timestampPeriod; } }
    pthread_t t; pthread_create(&t, NULL, completion_main, NULL); pthread_detach(t);
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    uint64_t end = timeout_ns == UINT64_MAX ? UINT64_MAX : mono_ns() + timeout_ns;
    pthread_mutex_lock(&g_cmu);
    while (atomic_load(&g_done) < fence) {
        if (end == UINT64_MAX) pthread_cond_wait(&g_ccv, &g_cmu);
        else { uint64_t now = mono_ns(); if (now >= end) break; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); uint64_t ns = ts.tv_nsec + (end - now);
               ts.tv_sec += ns / 1000000000ull; ts.tv_nsec = ns % 1000000000ull; pthread_cond_timedwait(&g_ccv, &g_cmu, &ts); }
    }
    pthread_mutex_unlock(&g_cmu);
    return atomic_load(&g_done) >= fence;
}
void mtl_finish(void) { mtl_wait(g_fence, UINT64_MAX); defer_drain(); }

// Arena: per-submission host-visible memory, 4 MB blocks (bigger requests get their own), 256-byte aligned; reclaimed when the submission's command pool is reset
static uint8_t *arena_alloc(sub_t *s, VkDeviceSize n, VkBuffer *buf, VkDeviceSize *off) {
    n = (n + 255) & ~(VkDeviceSize)255;
    arena_blk *b = s->nar ? &s->ar[s->nar - 1] : NULL;
    if (!b || b->used + n > b->size) {
        VkDeviceSize bs = n > (4u << 20) ? n : 4u << 20;
        for (int i = 0; i < s->nar; i++) if (s->ar[i].used == 0 && s->ar[i].size >= n) { b = &s->ar[i]; goto have; }   // never reached for now: blocks stay in order
        if (s->nar == s->car) { s->car = s->car ? s->car * 2 : 4; s->ar = realloc(s->ar, s->car * sizeof *s->ar); }
        b = &s->ar[s->nar++]; memset(b, 0, sizeof *b); b->size = bs;
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bs, .usage = BUF_ALL };
        VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &b->buf));
        VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, b->buf, &rq);
        b->a = vk_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
        VKCHECK(vkBindBufferMemory(g_dev, b->buf, b->a.b->mem, b->a.off)); b->map = (uint8_t *)b->a.b->map + b->a.off;
    }
have:
    *buf = b->buf; *off = b->used; b->used += n;
    return b->map + *off;
}
static void arena_reset(sub_t *s) {   // the submission's GPU work is done: big blocks go, the first 4 MB block stays for the next use
    for (int i = 0; i < s->nar; i++) {
        if (i == 0 && s->ar[i].size == (4u << 20)) { s->ar[i].used = 0; continue; }
        vkDestroyBuffer(g_dev, s->ar[i].buf, NULL); vk_free(s->ar[i].a);
    }
    s->nar = s->nar && s->ar[0].size == (4u << 20) && s->ar[0].buf ? 1 : 0;
}
static VkCommandBuffer ucb(void);   // the upload command buffer of the current submission (or of a one-off one)
static void pass_end(void);
static void state_reset(void);
static uint64_t g_frame_no;
void mtl_sub_begin(void) {
    mtl_init();
    if (g_sub) VDIE("submit re-entered");
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    sub_t *s = &g_subs[g_sub_next]; g_sub_next = (g_sub_next + 1) % NSUB;
    if (s->fence) mtl_wait(s->fence, UINT64_MAX);
    defer_drain();
    VKCHECK(vkResetCommandPool(g_dev, s->pool, 0)); arena_reset(s);
    for (int i = 0; i < s->ndp; i++) vkResetDescriptorPool(g_dev, s->dp[i], 0); s->cdp = 0;
    s->npost = 0; s->ucb_open = 0;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(s->cb, &bi));
    if (g_tsq) { int i = (int)(s - g_subs); s->frame = g_frame_no; unsigned k = s->frame % GRING;
        if (atomic_load(&g_gpu[k].frame) != s->frame) { atomic_store(&g_gpu[k].ns, 0); atomic_store(&g_gpu[k].pending, 0); atomic_store(&g_gpu[k].frame, s->frame); }
        atomic_fetch_add(&g_gpu[k].pending, 1);
        vkCmdResetQueryPool(s->cb, g_tsq, (uint32_t)i * 2, 2); vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, (uint32_t)i * 2); }
    g_sub = s;
    static int inited; if (!inited) { inited = 1; state_reset(); }
}
static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void queue_submit(VkCommandBuffer *cbs, uint32_t n, uint64_t signal) {
    VkTimelineSemaphoreSubmitInfo ts = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signal };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .commandBufferCount = n, .pCommandBuffers = cbs, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_tl };
    shadow_sync();
    pthread_mutex_lock(&g_qmu); VkResult r = vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&g_qmu);
    if (r) VDIE("vkQueueSubmit failed: %d", r);
}
// Fence values are handed out under g_cmu so the present thread's submissions (screenshots) and the game's stay in order on the timeline
static uint64_t submit_tracked(sub_t *s, VkCommandBuffer *cbs, uint32_t n) {
    pthread_mutex_lock(&g_cmu);
    uint64_t f = ++g_fence; s->fence = f;
    queue_submit(cbs, n, f);
    g_inflight[g_nin++] = s; pthread_cond_broadcast(&g_ccv);
    pthread_mutex_unlock(&g_cmu);
    return f;
}
uint64_t mtl_sub_commit(void) {
    sub_t *s = g_sub; if (!s) return g_fence;
    pass_end();
    if (g_tsq) vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, (uint32_t)(s - g_subs) * 2 + 1);
    VKCHECK(vkEndCommandBuffer(s->cb));
    VkCommandBuffer cbs[2]; uint32_t n = 0;
    if (s->ucb_open) { barrier_all(s->ucb); VKCHECK(vkEndCommandBuffer(s->ucb)); cbs[n++] = s->ucb; s->ucb_open = 0; }
    cbs[n++] = s->cb;
    g_sub = NULL;
    return submit_tracked(s, cbs, n);
}
void mtl_drain(void) { mtl_finish(); }
static void post(post_op o) {
    sub_t *s = g_sub; if (!s) { RED_ONCE(1, "event / counter outside a submission"); return; }
    if (s->npost == s->cpost) { s->cpost = s->cpost ? s->cpost * 2 : 64; s->post = realloc(s->post, s->cpost * sizeof *s->post); }
    s->post[s->npost++] = o;
}
// One-off command buffer for work outside a submission (uploads requested off the submit thread, readbacks): recorded, submitted and waited for at once
static VkCommandPool g_once_pool; static pthread_mutex_t g_once_mu = PTHREAD_MUTEX_INITIALIZER;
static VkCommandBuffer once_begin(void) {
    pthread_mutex_lock(&g_once_mu);
    if (!g_once_pool) { VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf }; VKCHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_once_pool)); }
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_once_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; VKCHECK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(cb, &bi)); barrier_all(cb);
    return cb;
}
static void once_end(VkCommandBuffer cb) {   // ordered after everything submitted so far (same queue, full barriers), waited for here
    barrier_all(cb); VKCHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f; VKCHECK(vkCreateFence(g_dev, &fi, NULL, &f));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    shadow_sync();
    pthread_mutex_lock(&g_qmu); VkResult r = vkQueueSubmit(g_q, 1, &si, f); pthread_mutex_unlock(&g_qmu);
    if (r) VDIE("one-off submit failed: %d", r);
    vkWaitForFences(g_dev, 1, &f, VK_TRUE, UINT64_MAX); vkDestroyFence(g_dev, f, NULL);
    vkFreeCommandBuffers(g_dev, g_once_pool, 1, &cb);
    pthread_mutex_unlock(&g_once_mu);
}
static VkCommandBuffer ucb(void) {
    sub_t *s = g_sub;
    if (!s || !on_sub_thread()) return VK_NULL_HANDLE;
    if (!s->ucb_open) { VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; VKCHECK(vkBeginCommandBuffer(s->ucb, &bi)); s->ucb_open = 1; }
    return s->ucb;
}

// ================= Textures =================
typedef struct { uint32_t fmt, vt, lvl, nlvl, lay, nlay, swz, aspect; } vview_key;
typedef struct { vview_key k; VkImageView v; } vview;
typedef struct vtex {
    _Atomic int refs; VkImage img; vmalloc_t a; VkFormat fmt; int nvn_fmt;
    uint32_t w, h, d, levels, layers, samples; VkImageUsageFlags usage; VkImageAspectFlags aspect; int is3d, cube_ok;
    uint8_t fswz[4];                       // the format's own channel mapping (no alpha, r/b swap), NVN_SWZ_*
    vview *views; int nviews; pthread_mutex_t vmu;
    int pkg; uint64_t stamp, seen; const void *span;   // CPU-filled: write-tracking stamp of the uploaded content
    VkBufferView bview;                    // buffer texture (TARGET_BUFFER): a view of its memory pool
    int fixed_fmt; VkFormat twin;          // 1: not mutable (depth / stencil), 2: mutable within fmt / twin only (tex_make: keeps UBWC)
} vtex;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;   // serial / sharing / CPU-written tables
static nvn_hmap g_by_serial = {.klen = 8}, g_ghost = {.klen = 8}, g_tex_live = {.klen = 8}, g_cpu_tex = {.klen = 8};
enum { NGHOST = 512 }; static uint64_t g_ghost_ring[NGHOST]; static unsigned g_ghost_next;
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};
static _Atomic uint64_t g_ser_gen = 1;
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_up_bytes;

static void vtex_ref(vtex *t) { if (t) atomic_fetch_add(&t->refs, 1); }
static atomic_long g_live_tex, g_live_tex_bytes;   // memory accounting (mem_report)
static void vtex_unref(vtex *t) {
    if (!t || atomic_fetch_sub(&t->refs, 1) != 1) return;
    atomic_fetch_sub(&g_live_tex, 1); atomic_fetch_sub(&g_live_tex_bytes, (long)t->a.size);
    for (int i = 0; i < t->nviews; i++) defer_add((dfr){ .kind = DF_VIEW, .h.view = t->views[i].v });
    if (t->bview) defer_add((dfr){ .kind = DF_BUFVIEW, .h.bview = t->bview });
    if (t->img) defer_add((dfr){ .kind = DF_IMAGE, .h.img = t->img, .a = t->a });
    free(t->views); free(t);
}
static int fmt_is_depth(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT; }
static int fmt_has_stencil(VkFormat f) { return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT; }
static int fmt_compressed(int f) { return (f >= 0x41 && f <= 0x50) || (f >= 0x79 && f <= 0x94); }
static VkImageAspectFlags fmt_aspect(VkFormat f) { return f == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : fmt_is_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT | (fmt_has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT; }

// Integer colour formats: a float clear colour is converted to integers (the hardware converts it; Vulkan reads the clear value's bits in the format's class)
static int fmt_int_class(VkFormat f) {   // 0 float / normalized, 1 uint, 2 sint
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 1;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32: return 2;
        default: return 0;
    }
}
static VkClearColorValue clear_for(VkFormat f, const float rgba[4]) {
    VkClearColorValue v; int k = fmt_int_class(f);
    for (int i = 0; i < 4; i++) { if (k == 1) v.uint32[i] = rgba[i] <= 0 ? 0 : (uint32_t)rgba[i]; else if (k == 2) v.int32[i] = (int32_t)rgba[i]; else v.float32[i] = rgba[i]; }
    return v;
}
// First use of a new image: to GENERAL (where every image stays), cleared to 0 when asked (new Switch memory reads 0; a new Vulkan image is undefined)
static void img_init(vtex *t, int zero) {
    VkCommandBuffer cb = ucb(); int once = !cb; if (once) cb = once_begin();
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = t->img, .subresourceRange = { t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    if (zero && t->samples <= 1) {
        VkImageSubresourceRange r = { t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        if (t->aspect & VK_IMAGE_ASPECT_COLOR_BIT) { VkClearColorValue c = {{0}}; vkCmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r); }
        else { VkClearDepthStencilValue c = {0, 0}; vkCmdClearDepthStencilImage(cb, t->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r); }
    }
    if (once) once_end(cb);
}
static vtex *tex_new(const tex_t *t, int pkg) {
    uint32_t vf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_vk(t->d.format, &vf, swz) || !vf) { RED_ONCE(2, "texture format %#x has no Vulkan equivalent (first case)", t->d.format); return NULL; }
    vtex *x = calloc(1, sizeof *x); atomic_fetch_add(&g_live_tex, 1); x->refs = 1; x->fmt = (VkFormat)vf; x->nvn_fmt = t->d.format; memcpy(x->fswz, swz, 4); pthread_mutex_init(&x->vmu, NULL);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = x->fmt, .mipLevels = t->d.levels ? t->d.levels : 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT };
    ci.extent = (VkExtent3D){ t->d.width ? t->d.width : 1, t->d.height ? t->d.height : 1, 1 };
    switch (t->d.target) {
        case 0: ci.extent.height = 1; break;                                                   // 1D as one-row 2D (as nvn_mtl.m)
        case 2: ci.imageType = VK_IMAGE_TYPE_3D; ci.extent.depth = t->d.depth ? t->d.depth : 1; ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; x->is3d = 1; break;
        case 3: ci.arrayLayers = t->d.height ? t->d.height : 1; ci.extent.height = 1; break;
        case 4: ci.arrayLayers = t->d.depth ? t->d.depth : 1; break;
        case 5: ci.samples = (VkSampleCountFlagBits)(t->d.samples ? t->d.samples : 1); ci.mipLevels = 1; break;
        case 6: ci.samples = (VkSampleCountFlagBits)(t->d.samples ? t->d.samples : 1); ci.arrayLayers = t->d.depth ? t->d.depth : 1; ci.mipLevels = 1; break;
        case 8: ci.arrayLayers = 6; break;
        case 9: ci.arrayLayers = (t->d.depth ? t->d.depth : 1) * 6; break;
        default: break;
    }
    if (!x->is3d && ci.arrayLayers >= 6 && ci.extent.width == ci.extent.height && ci.samples == VK_SAMPLE_COUNT_1_BIT) { ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; x->cube_ok = 1; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, x->fmt, &fp); VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
    if (!fmt_compressed(t->d.format)) {   // packaged textures too: a game may draw into one it loaded (Shield: an RGBA8 one, "cannot be attached", its pass lost)
        if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (fmt_compressed(t->d.format) && !nvn_fmt_vk_decode(t->d.format)) ci.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    // Mutable formats switch off Adreno's framebuffer compression (UBWC) unless the formats are listed: depth / stencil images are not mutable at all (their views keep
    // the format), colour images that can be render targets list their format and its sRGB / linear twin (the reinterpretations NVN views make of render targets)
    static int nolist = -1; if (nolist < 0) nolist = getenv("DM_VK_NO_FMT_LIST") != NULL;
    VkFormat twin = x->fmt == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : x->fmt == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM :
                    x->fmt == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : x->fmt == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : x->fmt;
    VkFormat fl[2] = { x->fmt, twin }; VkImageFormatListCreateInfo fli = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = twin != x->fmt ? 2 : 1, .pViewFormats = fl };
    // Mali (Arm) keeps AFBC only on images that are not mutable (Bifrost) or list their view formats (Valhall): there every colour image that is not block-compressed
    // gets the same treatment as render targets do on Adreno, and one with no sRGB / linear twin is not mutable at all
    static int arm = -1; if (arm < 0) arm = g_props.vendorID == 0x13B5 || getenv("DM_VK_ARM_FORMATS") != NULL;
    int plain = !fmt_compressed(t->d.format) || nvn_fmt_vk_decode(t->d.format);
    if (!nolist && !x->is3d) {
        if (fmt_is_depth(x->fmt) || x->fmt == VK_FORMAT_S8_UINT) { ci.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; x->fixed_fmt = 1; }
        else if (arm && plain && twin == x->fmt) { ci.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; x->fixed_fmt = 1; }
        else if ((ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || (arm && plain)) { ci.pNext = &fli; x->fixed_fmt = 2; x->twin = twin; }
    }
    x->w = ci.extent.width; x->h = ci.extent.height; x->d = ci.extent.depth; x->levels = ci.mipLevels; x->layers = ci.arrayLayers; x->samples = ci.samples; x->usage = ci.usage; x->aspect = fmt_aspect(x->fmt);
    VkResult r = vkCreateImage(g_dev, &ci, NULL, &x->img);
    if (r) { RED_ONCE(3, "vkCreateImage %ux%ux%u format %#x (Vulkan %u) target %d failed: %d", x->w, x->h, x->d, t->d.format, vf, t->d.target, r); free(x); return NULL; }
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, x->img, &rq);
    x->a = vk_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); atomic_fetch_add(&g_live_tex_bytes, (long)x->a.size);
    VKCHECK(vkBindImageMemory(g_dev, x->img, x->a.b->mem, x->a.off));
    static int logged;
    if (p_layout2 && (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && logged < 8) {   // the first render targets: is their framebuffer compression on?
        VkImageCompressionPropertiesEXT cp = { .sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT };
        VkSubresourceLayout2EXT sl = { .sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_EXT, .pNext = &cp };
        VkImageSubresource2EXT sr = { .sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_EXT, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 } };
        p_layout2(g_dev, x->img, &sr, &sl); logged++;
        VLOG("render target %ux%u format %d: framebuffer compression %s (flags %#x)", x->w, x->h, x->fmt,
             cp.imageCompressionFlags & VK_IMAGE_COMPRESSION_DISABLED_EXT ? "OFF" : "on", (unsigned)cp.imageCompressionFlags);
    }
    return x;
}
static VkImageView view_get(vtex *t, VkImageViewType vt, VkFormat fmt, uint32_t lvl, uint32_t nlvl, uint32_t lay, uint32_t nlay, const uint8_t sw[4], VkImageAspectFlags aspect) {
    if (t->fixed_fmt && fmt != t->fmt && fmt != t->twin) { RED_ONCE(73, "a view in format %d of an image whose formats are fixed to %d / %d: viewed in its own format (DM_VK_NO_FMT_LIST=1 to compare)", fmt, t->fmt, t->twin); fmt = t->fmt; }
    vview_key k = { (uint32_t)fmt, (uint32_t)vt, lvl, nlvl, lay, nlay, (uint32_t)sw[0] | (uint32_t)sw[1] << 8 | (uint32_t)sw[2] << 16 | (uint32_t)sw[3] << 24, (uint32_t)aspect };
    pthread_mutex_lock(&t->vmu);
    for (int i = 0; i < t->nviews; i++) if (!memcmp(&t->views[i].k, &k, sizeof k)) { VkImageView v = t->views[i].v; pthread_mutex_unlock(&t->vmu); return v; }
    VkImageViewUsageCreateInfo uci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = t->usage };
    // a view in a format whose features lack some of the image's usages (sRGB views of a storage image): the view only gets what its format supports
    if (fmt != t->fmt) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, fmt, &fp); VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
        if (!(ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) uci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
        if (!(ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) uci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) uci.usage &= ~VK_IMAGE_USAGE_SAMPLED_BIT; }
    VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &uci, .image = t->img, .viewType = vt, .format = fmt,
        .components = { (VkComponentSwizzle)sw[0], (VkComponentSwizzle)sw[1], (VkComponentSwizzle)sw[2], (VkComponentSwizzle)sw[3] },
        .subresourceRange = { aspect, lvl, nlvl, lay, nlay } };
    VkImageView v = VK_NULL_HANDLE; VkResult r = vkCreateImageView(g_dev, &ci, NULL, &v);
    if (r) { RED_ONCE(4, "vkCreateImageView type %d format %d (image format %d %ux%ux%u layers %u) levels %u+%u layers %u+%u failed: %d", vt, fmt, t->fmt, t->w, t->h, t->d, t->layers, lvl, nlvl, lay, nlay, r); v = VK_NULL_HANDLE; }
    else { t->views = realloc(t->views, (t->nviews + 1) * sizeof *t->views); t->views[t->nviews++] = (vview){k, v}; }
    pthread_mutex_unlock(&t->vmu);
    return v;
}

// CPU-filled texture upload: deswizzled level by level / layer by layer into staging (the submission's arena, else a one-off buffer), copied into the image in the upload command buffer
static int tex_upload(vtex *x, const tex_t *t) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb) return -1;
    // BC on a GPU without it: the blocks are deswizzled into scratch and decoded into staging as plain texels (bc_decode.c)
    int bck = nvn_fmt_vk_decode(t->d.format), obpb = bck ? bc_texel_bytes(bck) : bpb, obw = bck ? 1 : bw, obh = bck ? 1 : bh; uint8_t *scratch = NULL;
    unsigned layers = x->is3d ? 1 : x->layers;
    VkDeviceSize total = 0;
    for (unsigned l = 0; l < x->levels; l++) { uint32_t w = x->w >> l ? x->w >> l : 1, h = x->h >> l ? x->h >> l : 1, z = x->is3d ? (x->d >> l ? x->d >> l : 1) : 1;
        total += (VkDeviceSize)((w + obw - 1) / obw) * obpb * ((h + obh - 1) / obh) * z * layers; }
    VkCommandBuffer cb = ucb(); int once = !cb; if (once) cb = once_begin();
    VkBuffer sb; VkDeviceSize so = 0; uint8_t *sp; vmalloc_t tmp = {0};
    if (!once) sp = arena_alloc(g_sub, total ? total : 16, &sb, &so);
    else { VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total ? total : 16, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT }; VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &sb));
           VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, sb, &rq); tmp = vk_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
           VKCHECK(vkBindBufferMemory(g_dev, sb, tmp.b->mem, tmp.off)); sp = (uint8_t *)tmp.b->map + tmp.off; }
    VkBufferImageCopy *rg = calloc(x->levels * layers, sizeof *rg); uint32_t nrg = 0; VkDeviceSize off = 0; int linear = !!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
    for (unsigned l = 0; l < x->levels; l++) {
        uint32_t w = x->w >> l ? x->w >> l : 1, h = x->h >> l ? x->h >> l : 1, z = x->is3d ? (x->d >> l ? x->d >> l : 1) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh, opitch = (size_t)((w + obw - 1) / obw) * obpb, orows = (h + obh - 1) / obh;
        if (bck) { free(scratch); scratch = malloc(pitch * rows * z); }
        for (unsigned y = 0; y < layers; y++) {
            uint8_t *dst = bck ? scratch : sp + off;
            if (linear) {   // LINEAR: rows at the texture's stride (one level, one layer)
                size_t stride = t->d.stride ? t->d.stride : pitch; for (size_t r = 0; r < rows; r++) memcpy(dst + r * pitch, t->mem + r * stride, pitch);
            } else if (nvn_deswizzle(&t->d, l, y, t->mem, dst, pitch)) { RED_ONCE(5, "deswizzle failed format %#x target %d", t->d.format, t->d.target); break; }
            if (bck) for (uint32_t s = 0; s < z; s++) bc_decode_image(bck, scratch + s * pitch * rows, pitch, (int)w, (int)h, sp + off + s * opitch * orows, opitch);
            rg[nrg++] = (VkBufferImageCopy){ .bufferOffset = so + off, .bufferRowLength = (uint32_t)(opitch / obpb * obw), .bufferImageHeight = (uint32_t)(orows * obh),
                .imageSubresource = { x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : x->aspect, l, x->is3d ? 0 : y, 1 }, .imageExtent = { w, h, z } };
            off += opitch * orows * z;
        }
    }
    free(scratch);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .image = x->img, .subresourceRange = { x->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    if (nrg) vkCmdCopyBufferToImage(cb, sb, x->img, VK_IMAGE_LAYOUT_GENERAL, nrg, rg);
    free(rg); g_up_bytes += total;
    if (once) { once_end(cb); vkDestroyBuffer(g_dev, sb, NULL); vk_free(tmp); }
    return 0;
}
static vtex *pkg_upload(const tex_t *t) {
    vtex *x = tex_new(t, 1); if (!x) return NULL;
    nvn_dirty_protect(t->mem, t->L.size); x->stamp = nvn_dirty_now();
    if (tex_upload(x, t)) { vtex_unref(x); return NULL; }
    x->pkg = 1; x->span = nvn_dirty_span(t->mem, t->L.size); x->seen = nvn_dirty_now();
    return x;
}
static vtex *serial_get(uint64_t k) {   // caller holds g_smu
    vtex *x = nvn_hm_get(&g_by_serial, &k); if (!x && g_ghost.n) x = nvn_hm_get(&g_ghost, &k); return x;
}
static void serial_put(uint64_t k, vtex *x) {   // takes a reference for the table; caller holds g_smu
    nvn_hmap *m = nvn_hm_get(&g_tex_live, &k) ? &g_by_serial : &g_ghost;
    vtex_ref(x); vtex *old = nvn_hm_put(m, &k, x);
    if (old) vtex_unref(old);
    else if (m == &g_ghost) { uint64_t *slot = &g_ghost_ring[g_ghost_next++ % NGHOST]; if (*slot) { vtex *ev = nvn_hm_del(&g_ghost, slot); if (ev) vtex_unref(ev); } *slot = k; }
    g_ser_gen++;
}
void mtl_tex_init(const tex_t *t) { if (!t->serial) return; pthread_mutex_lock(&g_smu); nvn_hm_put(&g_tex_live, &t->serial, (void *)1); pthread_mutex_unlock(&g_smu); }
void mtl_tex_release(tex_t *t) {
    if (!t->serial) return;
    pthread_mutex_lock(&g_smu); nvn_hm_del(&g_tex_live, &t->serial); vtex *x = nvn_hm_del(&g_by_serial, &t->serial); if (x) { vtex_unref(x); g_ser_gen++; } pthread_mutex_unlock(&g_smu);
}
void mtl_tex_cpu_written(const tex_t *t) {
    if (!t->serial) return;
    pthread_mutex_lock(&g_smu); if (!nvn_hm_get(&g_cpu_tex, &t->serial)) { nvn_hm_put(&g_cpu_tex, &t->serial, (void *)1); g_ser_gen++; } pthread_mutex_unlock(&g_smu);
}
int mtl_tex_gpu_owned(uint64_t serial) {
    pthread_mutex_lock(&g_smu); vtex *x = serial_get(serial); int r = x && !x->pkg && (x->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)); pthread_mutex_unlock(&g_smu);
    return r;
}
void mtl_alias_drop_pool(const void *pool) {
    pthread_mutex_lock(&g_smu);
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)pool) continue;
        vtex *x = nvn_hm_del(&g_alias, &k); if (x) vtex_unref(x); i = (uint32_t)-1;   // deletion shifts entries: rescan
    }
    g_ser_gen++;
    pthread_mutex_unlock(&g_smu);
}
// Texture object → its image (nvn_mtl.m tex_mtl_): CPU-filled ones (packaged layout, CPU-written, linear) uploaded and re-uploaded when their pages change; buffer textures as views of
// their pool; the rest are GPU images shared by storage + description. The returned image is valid until the submission completes (tables hold it, replacements are deferred)
static vtex *tex_res(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) { RED_ONCE(6, "texture object no longer valid (magic %#x, ser=%llu)", t->magic, (unsigned long long)t->serial); return NULL; }
    pthread_mutex_lock(&g_smu);
    int linear = (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && t->d.target != NVN_TEXTURE_TARGET_BUFFER && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS);
    int packaged = (t->has_layout || nvn_hm_get(&g_cpu_tex, &t->serial) || linear) && t->mem && t->d.target != NVN_TEXTURE_TARGET_BUFFER;
    vtex *have = serial_get(t->serial);
    if (packaged) {
        if (have && have->pkg) {
            uint64_t now = nvn_dirty_now();
            int clean = have->span && (now == have->seen || !nvn_dirty_since_span(have->span, t->mem, t->L.size, have->stamp));
            have->seen = now;
            if (clean) { pthread_mutex_unlock(&g_smu); return have; }
            { static int nl; if (nl < 400 && getenv("DM_VK_REUP")) { nl++; VLOG("re-upload ser=%llu %ux%u format %#x target %d flags %#x layout %d pool flags %#x span %p seen %llu now %llu", (unsigned long long)t->serial, t->d.width, t->d.height,
                t->d.format, t->d.target, t->d.flags, t->has_layout, t->pool ? t->pool->flags : 0, have->span, (unsigned long long)have->seen, (unsigned long long)now); } }
            vtex *nt = pkg_upload(t);   // content changed: a new image (earlier draws keep the old one until they complete)
            if (nt) { serial_put(t->serial, nt); vtex_unref(nt); g_pkg_reuploads++; pthread_mutex_unlock(&g_smu); return nt; }
            pthread_mutex_unlock(&g_smu); return have;
        }
        vtex *nt = pkg_upload(t);
        if (nt) { serial_put(t->serial, nt); vtex_unref(nt); g_pkg_uploads++; }
        pthread_mutex_unlock(&g_smu); return nt;
    }
    if (have) { pthread_mutex_unlock(&g_smu); return have; }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) {
        uint32_t vf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        VkBuffer b; VkDeviceSize o, left;
        if (!nvn_fmt_to_vk(t->d.format, &vf, swz) || !vf || !bpb || !t->mem || !pool_buf((uint64_t)(uintptr_t)t->mem, (uint64_t)t->d.width * bpb, &b, &o, &left)) {
            RED_ONCE(7, "buffer texture format %#x / storage not usable", t->d.format); pthread_mutex_unlock(&g_smu); return NULL; }
        if (o % g_props.limits.minTexelBufferOffsetAlignment) { RED_ONCE(8, "buffer texture offset %#llx not aligned to %llu", (unsigned long long)o, (unsigned long long)g_props.limits.minTexelBufferOffsetAlignment); pthread_mutex_unlock(&g_smu); return NULL; }
        vtex *x = calloc(1, sizeof *x); atomic_fetch_add(&g_live_tex, 1); x->refs = 1; x->fmt = (VkFormat)vf; x->nvn_fmt = t->d.format; pthread_mutex_init(&x->vmu, NULL);
        VkDeviceSize n = (VkDeviceSize)t->d.width * bpb; if (n > left) n = left;
        VkBufferViewCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = x->fmt, .offset = o, .range = n };
        if (vkCreateBufferView(g_dev, &bci, NULL, &x->bview)) { RED_ONCE(9, "buffer texture view failed (format %u, %llu bytes)", vf, (unsigned long long)n); free(x); pthread_mutex_unlock(&g_smu); return NULL; }
        serial_put(t->serial, x); vtex_unref(x); pthread_mutex_unlock(&g_smu); return x;
    }
    alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d;
    vtex *shared = nvn_hm_get(&g_alias, &ak);
    if (shared) { serial_put(t->serial, shared); pthread_mutex_unlock(&g_smu); return shared; }
    vtex *x = tex_new(t, 0);
    if (!x) { pthread_mutex_unlock(&g_smu); return NULL; }
    img_init(x, !(x->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || 1);
    nvn_hm_put(&g_alias, &ak, x);                 // the sharing table holds the creation reference
    serial_put(t->serial, x);
    pthread_mutex_unlock(&g_smu);
    return x;
}

// ---------- Texture / sampler registries (descriptor pools) ----------
typedef struct { tex_t t; int ok; view_t v; int has_v; } treg_t;
static treg_t *g_treg; static int g_ntreg; static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static VkSampler *g_svk; static int g_nsreg;
static VkSampler *g_svkn; static int g_nsvkn;   // unfiltered twins for integer textures (samp_nearest_by_id)
static int g_tpool_n;
void mtl_set_tex_pool_n(int n) { g_tpool_n = n; }
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    if (tid < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (tid >= g_ntreg) { int n = tid + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_treg[tid].ok = t != NULL; if (t) g_treg[tid].t = *t; g_treg[tid].has_v = v != NULL; if (v) g_treg[tid].v = *v;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    if (sid < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (sid >= g_nsreg) { int n = sid + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); g_svk = realloc(g_svk, n * sizeof *g_svk);
        memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); memset(g_svk + g_nsreg, 0, (n - g_nsreg) * sizeof *g_svk); g_nsreg = n; }
    if (g_svk[sid]) { defer_add((dfr){ .kind = DF_ALLOC }); /* samplers are tiny: the old one is kept (in-flight descriptors may name it) */ g_svk[sid] = VK_NULL_HANDLE; }
    if (sid < g_nsvkn) g_svkn[sid] = VK_NULL_HANDLE;
    g_sreg[sid] = *sp; g_sreg_ok[sid] = 1;
    pthread_mutex_unlock(&g_rmu);
}
static VkSamplerAddressMode wrapm(int w) {
    switch (w) { case 0: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
                 case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
static VkSampler samp_vk(const samp_par *p) {
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (p->minf == 1 || p->minf == 3 || p->minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = p->minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrapm(p->ws); ci.addressModeV = wrapm(p->wt); ci.addressModeW = wrapm(p->wr);
    ci.minLod = p->lod_min < 0 ? 0 : p->lod_min; ci.maxLod = p->minf <= 1 ? 0.25f : p->lod_max; ci.mipLodBias = p->lod_bias;   // no mip filter: the base level only (maxLod 0.25, as GL on Vulkan)
    if (p->aniso > 1) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = p->aniso > g_props.limits.maxSamplerAnisotropy ? g_props.limits.maxSamplerAnisotropy : p->aniso; }
    if (p->cmp_mode) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)(p->cmp_func - 1); }
    VkSamplerReductionModeCreateInfo rd = { .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, .reductionMode = p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX };
    VkSamplerCustomBorderColorCreateInfoEXT bc = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT, .format = VK_FORMAT_UNDEFINED };
    const void **tail = &ci.pNext;
    if (p->reduction == 1 || p->reduction == 2) { *tail = &rd; tail = &rd.pNext; }
    int std = (p->border[0] == 0 && p->border[1] == 0 && p->border[2] == 0 && (p->border[3] == 0 || p->border[3] == 1)) || (p->border[0] == 1 && p->border[1] == 1 && p->border[2] == 1 && p->border[3] == 1);
    if (g_border && !std) { memcpy(bc.customBorderColor.float32, p->border, 16); ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; *tail = &bc; }
    else ci.borderColor = p->border[3] > 0.5f ? (p->border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK) : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE; if (vkCreateSampler(g_dev, &ci, NULL, &s)) RED_ONCE(10, "vkCreateSampler failed");
    return s;
}
// The same sampler without filtering, for integer-format textures (Vulkan cannot filter them: undefined on strict drivers). Made on first use, kept per ID
static VkSampler samp_nearest_by_id(int sid) {   // caller holds g_rmu
    if (sid < 0 || sid >= g_nsreg || !g_sreg_ok[sid]) return VK_NULL_HANDLE;
    if (sid >= g_nsvkn) { int n = g_nsreg; g_svkn = realloc(g_svkn, n * sizeof *g_svkn); memset(g_svkn + g_nsvkn, 0, (n - g_nsvkn) * sizeof *g_svkn); g_nsvkn = n; }
    if (!g_svkn[sid]) { samp_par q = g_sreg[sid]; q.magf = 0; q.minf = q.minf <= 1 ? 0 : 2; q.aniso = 0; g_svkn[sid] = samp_vk(&q); }
    return g_svkn[sid];
}
// Sampler ID 0 is a real sampler, as in the Metal replay (Pokémon Legends: Arceus samples through it: treated as unbound it fell back to the unfiltered dummy and every gradient banded)
static VkSampler samp_by_id(int sid) {   // caller holds g_rmu
    if (sid < 0 || sid >= g_nsreg || !g_sreg_ok[sid]) return VK_NULL_HANDLE;
    if (!g_svk[sid]) g_svk[sid] = samp_vk(&g_sreg[sid]);
    return g_svk[sid];
}

// ================= Dummies (unbound slots read 0, as on the Switch) =================
static vtex *g_dummy[8][3];   // [view kind][class]: 0 2D, 1 2D array, 2 cube, 3 cube array, 4 3D, 5 2D multisample; class 0 float 1 uint 2 sint
static vtex *g_dummy_depth[4];
static VkBuffer g_zero_buf; static vmalloc_t g_zero_a; static VkBuffer g_support_buf; static vmalloc_t g_support_a;
static VkSampler g_dummy_samp; static VkBufferView g_dummy_tbuf[3];
static vtex *dummy_make(VkFormat f, VkImageType type, uint32_t layers, int cube, VkSampleCountFlagBits samples, int storage) {
    vtex *x = calloc(1, sizeof *x); atomic_fetch_add(&g_live_tex, 1); x->refs = 1; x->fmt = f; pthread_mutex_init(&x->vmu, NULL);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = type, .format = f, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = layers, .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0), .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0 };
    if (samples > 1) ci.usage |= fmt_is_depth(f) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VKCHECK(vkCreateImage(g_dev, &ci, NULL, &x->img));
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, x->img, &rq); x->a = vk_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); atomic_fetch_add(&g_live_tex_bytes, (long)x->a.size);
    VKCHECK(vkBindImageMemory(g_dev, x->img, x->a.b->mem, x->a.off));
    x->w = x->h = x->d = 1; x->levels = 1; x->layers = layers; x->samples = samples; x->usage = ci.usage; x->aspect = fmt_aspect(f); x->is3d = type == VK_IMAGE_TYPE_3D; x->cube_ok = cube;
    x->fswz[0] = NVN_SWZ_R; x->fswz[1] = NVN_SWZ_G; x->fswz[2] = NVN_SWZ_B; x->fswz[3] = NVN_SWZ_A;
    VkCommandBuffer cb = once_begin(); {
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = x->img, .subresourceRange = { x->aspect, 0, 1, 0, layers } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkImageSubresourceRange r = { x->aspect, 0, 1, 0, layers };
        if (x->aspect & VK_IMAGE_ASPECT_COLOR_BIT) { VkClearColorValue c = {{0}}; vkCmdClearColorImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r); }
        else { VkClearDepthStencilValue c = {0, 0}; vkCmdClearDepthStencilImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r); } }
    once_end(cb);
    return x;
}
static VkBuffer buf_make(VkDeviceSize n, vmalloc_t *a, void **map) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = BUF_ALL }; VkBuffer b; VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &b));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, b, &rq); *a = vk_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VKCHECK(vkBindBufferMemory(g_dev, b, a->b->mem, a->off)); if (map) *map = (uint8_t *)a->b->map + a->off; return b;
}
static void dummies_init(void) {
    static const VkFormat F[3] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT };
    for (int c = 0; c < 3; c++) {
        g_dummy[0][c] = dummy_make(F[c], VK_IMAGE_TYPE_2D, 1, 0, VK_SAMPLE_COUNT_1_BIT, 1);
        g_dummy[1][c] = g_dummy[0][c];
        g_dummy[2][c] = dummy_make(F[c], VK_IMAGE_TYPE_2D, 6, 1, VK_SAMPLE_COUNT_1_BIT, 0);
        g_dummy[3][c] = g_dummy[2][c];
        g_dummy[4][c] = dummy_make(F[c], VK_IMAGE_TYPE_3D, 1, 0, VK_SAMPLE_COUNT_1_BIT, 0);
        g_dummy[5][c] = dummy_make(F[c], VK_IMAGE_TYPE_2D, 1, 0, VK_SAMPLE_COUNT_4_BIT, 0);
    }
    g_dummy_depth[0] = dummy_make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, 1, 0, VK_SAMPLE_COUNT_1_BIT, 0); g_dummy_depth[1] = g_dummy_depth[0];
    g_dummy_depth[2] = dummy_make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, 6, 1, VK_SAMPLE_COUNT_1_BIT, 0); g_dummy_depth[3] = g_dummy_depth[2];
    void *m; g_zero_buf = buf_make(65536, &g_zero_a, &m); memset(m, 0, 65536);
    // Ryujinx's support buffer (SupportBuffer.cs): alpha test 0, BGRA swaps 0, render scale 1.0 (float4 [73] from byte 192, .x read)
    g_support_buf = buf_make(65536, &g_support_a, &m); memset(m, 0, 65536); for (int i = 0; i < 73; i++) *(float *)((uint8_t *)m + 192 + 16 * i) = 1.0f;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 0.25f }; VKCHECK(vkCreateSampler(g_dev, &sci, NULL, &g_dummy_samp));
    for (int c = 0; c < 3; c++) { VkBufferViewCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = g_zero_buf, .format = F[c] == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R32G32B32A32_SFLOAT : F[c], .range = 65536 };
        VKCHECK(vkCreateBufferView(g_dev, &bci, NULL, &g_dummy_tbuf[c])); }
}
static const uint8_t SW_ID[4] = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
// shader image declaration → view kind (dummy table row) and view type
static int vkind(const spv_res *r) {
    if (r->ms) return 5;
    switch (r->dim) { case SPV_DIM_3D: return 4; case SPV_DIM_CUBE: return r->arrayed ? 3 : 2; default: return r->arrayed ? 1 : 0; }
}
static const VkImageViewType VK_OF_KIND[6] = { VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_2D };
static VkImageView dummy_view(const spv_res *r, int storage) {
    int k = vkind(r); vtex *x = r->depth && !storage && k <= 3 ? g_dummy_depth[k] : g_dummy[k][r->sclass < 3 ? r->sclass : 0];
    if (storage && k != 0 && k != 1) x = g_dummy[0][r->sclass < 3 ? r->sclass : 0], k = 0;
    uint32_t nl = k == 2 ? 6 : k == 3 ? 6 : 1;
    return view_get(x, VK_OF_KIND[k], x->fmt, 0, 1, 0, nl, SW_ID, x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : x->aspect);
}
// Texture ID → view of the kind the shader declares (nvn_mtl.m tex_by_id + tex_fit): the registered view's format / levels / layers, its channel mapping (else the builder's) on top of
// the format's own; a depth-stencil texture reads depth unless the view asks for stencil. VK_NULL_HANDLE: no fitting view (the slot reads 0)
static int tex_is_int(int tid) {   // the texture ID's view format is an integer one
    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    treg_t rg = g_treg[tid]; vtex *x = tex_res(&rg.t); if (!x) return 0;
    VkFormat f = x->fmt; if (rg.has_v && rg.v.set_format) { uint32_t vf = 0; uint8_t s2[4]; if (nvn_fmt_to_vk(rg.v.format, &vf, s2) && vf) f = (VkFormat)vf; }
    return fmt_int_class(f) != 0;
}
static VkImageView tex_view_for(int tid, const spv_res *r, int storage) {
    if (g_tpool_n && tid >= g_tpool_n) return VK_NULL_HANDLE;
    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) { if (tid > 0) RED_ONCE(11, "texture ID %d not registered", tid); return VK_NULL_HANDLE; }
    treg_t rg = g_treg[tid];
    vtex *x = tex_res(&rg.t); if (!x || !x->img) return VK_NULL_HANDLE;
    VkFormat f = x->fmt; uint8_t fsw[4]; memcpy(fsw, x->fswz, 4);
    if (rg.has_v && rg.v.set_format) { uint32_t vf = 0; uint8_t s2[4]; if (nvn_fmt_to_vk(rg.v.format, &vf, s2) && vf) { f = (VkFormat)vf; memcpy(fsw, s2, 4); } }
    uint32_t l0 = rg.has_v && rg.v.set_levels ? rg.v.base_level : 0, nl = rg.has_v && rg.v.set_levels && rg.v.nlevels ? rg.v.nlevels : x->levels - (l0 < x->levels ? l0 : 0);
    uint32_t s0 = rg.has_v && rg.v.set_layers ? rg.v.min_layer : 0, ns = rg.has_v && rg.v.set_layers && rg.v.nlayers ? rg.v.nlayers : x->layers - (s0 < x->layers ? s0 : 0);
    if (l0 >= x->levels || s0 >= x->layers) return VK_NULL_HANDLE;
    if (l0 + nl > x->levels) nl = x->levels - l0; if (s0 + ns > x->layers) ns = x->layers - s0;
    int k = vkind(r); VkImageViewType vt = VK_OF_KIND[k];
    if (k == 4 && !x->is3d) { RED_ONCE(12, "shader declares 3D, texture ID %d is not 3D: reads 0", tid); return VK_NULL_HANDLE; }
    if (k != 4 && x->is3d) { RED_ONCE(13, "shader declares a 2D-family image, texture ID %d is 3D: reads 0", tid); return VK_NULL_HANDLE; }
    if (k == 5 ? x->samples <= 1 : x->samples > 1) { RED_ONCE(14, "multisample mismatch for texture ID %d (shader ms %d, texture %u samples)", tid, k == 5, x->samples); return VK_NULL_HANDLE; }
    if (k == 0 || k == 5) ns = 1;
    // a cube view's layer count can be in cubes, not faces: Shield's environment cube is registered as 1 layer of a 6-face texture (read as 0, its water lost the
    // reflections and turned flat blue)
    if ((k == 2 || k == 3) && x->cube_ok && ns < 6 && x->layers - s0 >= 6) { uint32_t nc = ns ? ns : 1;
        static int said[64]; static int nsaid; int seen = 0; for (int q = 0; q < nsaid; q++) if (said[q] == tid) seen = 1;
        if (!seen && nsaid < 64) { said[nsaid++] = tid; VLOG("cube view of texture ID %d: %ux%u, %u layers from %u, %u levels, Vulkan format %d, %s, usage %#x, serial %llu", tid, x->w, x->h, x->layers, s0, x->levels, x->fmt,
            x->pkg ? "packaged (from the game's files)" : "made at run time", x->usage, (unsigned long long)rg.t.serial); } ns = nc * 6 <= x->layers - s0 ? nc * 6 : (x->layers - s0) / 6 * 6; }
    if (k == 2 || k == 3) { if (!x->cube_ok || ns < 6) { RED_ONCE(15, "shader declares a cube, texture ID %d has %u layers (cube-compatible %d): reads 0", tid, ns, x->cube_ok); return VK_NULL_HANDLE; } ns = k == 2 ? 6 : ns / 6 * 6; }
    uint8_t sw[4] = { NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A };
    if (rg.has_v && rg.v.set_swizzle) memcpy(sw, rg.v.swz, 4);
    else if (rg.t.set_swz) for (int c = 0; c < 4; c++) sw[c] = (uint8_t)(rg.t.swz >> (3 * c) & 7);
    uint8_t vk[4];
    for (int c = 0; c < 4; c++) {   // the user's mapping applied on top of the format's: a channel picks a format channel (whose own mapping may be zero / one)
        uint8_t s = sw[c] > NVN_SWZ_A ? (uint8_t)(NVN_SWZ_R + c) : sw[c];
        static int nofmt = -1; if (nofmt < 0) nofmt = getenv("DM_VK_NO_FMT_SWZ") != NULL;
        if (s >= NVN_SWZ_R && !nofmt) s = fsw[s - NVN_SWZ_R];
        vk[c] = (uint8_t)(s + 1);   // NVN_SWZ_ZERO 0 → VK_COMPONENT_SWIZZLE_ZERO 1, ONE → 2, R → 3 …
    }
    VkImageAspectFlags asp = x->aspect;
    if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) asp = rg.has_v && rg.v.set_dsmode && rg.v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL && fmt_has_stencil(x->fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    if (storage) { if (!(x->usage & VK_IMAGE_USAGE_STORAGE_BIT)) { RED_ONCE(16, "texture ID %d bound as an image lacks storage usage", tid); return VK_NULL_HANDLE; } memcpy(vk, SW_ID, 4); }
    return view_get(x, vt, f, l0, nl, s0, ns, vk, asp);
}

// ================= Shaders and programs =================
enum { W_NONE, W_SUPPORT, W_C1, W_DRV0, W_DRV2, W_CBUF, W_SSBO, W_UBOP, W_SBUFU, W_TEX, W_TEXU, W_IMG, W_TBUF };
enum { S_NONE, S_TEX, S_SEP, S_UBO, S_ID };   // where a combined texture's sampler comes from: the texture handle's high bits, a separate sampler slot, a handle in a uniform buffer, a constant ID
typedef struct { uint8_t what, skind, rw; uint16_t reg, off, sreg, soff; } vres_how;
typedef struct vshader {
    char key[24]; int stage; VkShaderStageFlagBits vkstage; VkShaderModule mod, glmod; uint32_t shift;
    spv_res res[64]; vres_how how[64]; int nres;
    spv_input in[32]; int nin;
    uint8_t drv_read[3]; uint8_t color_mask, color_known;
    VkBuffer c1buf; VkDeviceSize c1off, c1len;
    uint64_t in_mask, out_mask;                 // user varyings by location (spv_io_masks)
    uint32_t *code; size_t nw;                  // fragment shaders: the (rebound) SPIR-V, for variants
    uint8_t out_cls[8];                         // fragment outputs by location: 0 float, 1 uint, 2 sint, 255 none
    struct { uint64_t drop; uint32_t um, sm; VkShaderModule mod; } var[8]; int nvar;   // variants: inputs the previous stage never writes (drop), float outputs to integer targets (um / sm)
    int tf[4][2]; uint16_t tf_stride[4]; VkShaderModule tfmod;   // transform feedback: per buffer { output location, components } and stride; the capture variant (tf_draw)
} vshader;
static char g_spv_dir[1024];
static nvn_hmap g_shmap = {.klen = 8};      // shader key (8 bytes) → stage + 1 (as a pointer)
static nvn_hmap g_shcache = {.klen = 8};    // shader key → vshader
static int stage_index(const char *s) { return !strcmp(s, "vertex") ? 0 : !strcmp(s, "pixel") ? 1 : !strcmp(s, "compute") ? 5 : !strcmp(s, "hull") ? 3 : !strcmp(s, "domain") ? 4 : !strcmp(s, "geometry") ? 2 : -1; }
static int hexkey(const char *s, uint8_t k[8]) { for (int i = 0; i < 8; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; k[i] = (uint8_t)v; } return 0; }
static void shmap_read(void);
static void shmap_load(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, shmap_read); }   // the game thread (mtl_shader_seen) and the replay both ask
static void shmap_read(void) {
    const char *e = getenv("DM_NVN_SHMAP"); FILE *f = e ? fopen(e, "r") : NULL;
    if (!f) VDIE("cannot read the shader map (DM_NVN_SHMAP=%s)", e ? e : "(unset)");
    char line[1024]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !strncmp(line, "package", 7)) continue;
        char *c[5] = {0}; int nc = 0; for (char *p = line; nc < 5 && p; nc++) { c[nc] = p; p = strchr(p, '\t'); if (p) *p++ = 0; }
        if (nc < 4) continue; char *nl = strpbrk(c[3], "\r\n"); if (nl) *nl = 0;
        uint8_t k[8]; int st = stage_index(c[2]); if (st < 0 || strlen(c[3]) < 16 || hexkey(c[3], k)) continue;
        nvn_hm_put(&g_shmap, k, (void *)(intptr_t)(st + 1)); n++;
    }
    fclose(f);
    const char *d = getenv("DM_SPV_DIR"); snprintf(g_spv_dir, sizeof g_spv_dir, "%s/b-%s", d ? d : ".", DMSH_CHAIN);
    VLOG("shader map: %d shaders, SPIR-V from %s", n, g_spv_dir);
}
static void *file_read(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long l = ftell(f); rewind(f);
    void *b = malloc(l > 0 ? (size_t)l : 1); if (l > 0 && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); fclose(f); return NULL; } fclose(f); *n = (size_t)l; return b;
}
// The shader's own constant segment (c1): written once into a shared host-visible bank, bound at its exact length (reads past it give 0, as on the Switch)
static VkBuffer g_cbank; static uint8_t *g_cbank_map; static VkDeviceSize g_cbank_used, g_cbank_size; static pthread_mutex_t g_cbmu = PTHREAD_MUTEX_INITIALIZER;
static void c1_store(vshader *s, const uint8_t *c1, uint32_t n) {
    pthread_mutex_lock(&g_cbmu);
    VkDeviceSize need = (n + 255) & ~255u; if (!need) need = 256;
    if (!g_cbank || g_cbank_used + need > g_cbank_size) { vmalloc_t a; void *m; g_cbank_size = 4u << 20; g_cbank = buf_make(g_cbank_size, &a, &m); g_cbank_map = m; g_cbank_used = 0; memset(m, 0, g_cbank_size); }
    memcpy(g_cbank_map + g_cbank_used, c1, n); s->c1buf = g_cbank; s->c1off = g_cbank_used; s->c1len = n ? (n + 15) & ~15u : 16; g_cbank_used += need;
    pthread_mutex_unlock(&g_cbmu);
}
enum { TF_BIND = 320 };   // the capture buffers' bindings (past every stage's shifted range, stage_shift)
static uint32_t stage_shift(int st) { return st == 1 ? 64 : st == 2 ? 128 : st == 3 ? 192 : st == 4 ? 256 : 0; }
// The shader set packed into one mapped file (DM_SPV_PAK, dm/android/tools/spv_pak.py): "NXSP", version, count, chain[16], sorted index, data.
// Versions 1/2 index SPIR-V and reflection; versions 3/4 additionally index a GL-depth fallback SPIR-V. Versions 2/4 compress each blob.
static const uint8_t *g_pak; static size_t g_pak_n; static uint32_t g_pak_count, g_pak_ver;
static int pak_open(void) {
    static int tried; if (tried++) return g_pak != NULL;
    const char *path = getenv("DM_SPV_PAK"); if (!path) return 0;
    int fd = open(path, O_RDONLY); struct stat st; if (fd < 0 || fstat(fd, &st)) { VLOG("[RED] shader pack %s cannot be opened", path); if (fd >= 0) close(fd); return 0; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED || st.st_size < 28 || memcmp(m, "NXSP", 4)) { VLOG("[RED] shader pack %s is not one", path); return 0; }
    char chain[17] = {0}; memcpy(chain, (const uint8_t *)m + 12, 16);
    if (strcmp(chain, DMSH_CHAIN)) { VLOG("[RED] shader pack %s has chain %s, this host expects %s", path, chain, DMSH_CHAIN); munmap(m, (size_t)st.st_size); return 0; }
    g_pak = m; g_pak_n = (size_t)st.st_size; memcpy(&g_pak_ver, g_pak + 4, 4); memcpy(&g_pak_count, g_pak + 8, 4);
    if (g_pak_ver < 1 || g_pak_ver > 4) { VLOG("[RED] shader pack %s has version %u", path, g_pak_ver); g_pak = NULL; munmap(m, (size_t)st.st_size); return 0; }
    size_t entry_size = g_pak_ver >= 3 ? 44 : 32;
    if (g_pak_count > (g_pak_n - 28) / entry_size) { VLOG("[RED] shader pack %s has a truncated index", path); g_pak = NULL; munmap(m, (size_t)st.st_size); return 0; }
    VLOG("shader pack %s: %u shaders%s%s", path, g_pak_count, g_pak_ver == 2 || g_pak_ver == 4 ? " (compressed)" : "", g_pak_ver >= 3 ? " (GL depth fallback)" : "");
    return 1;
}
static void *pak_blob(const uint8_t *p, size_t n, size_t *out) {   // a malloc'd copy of one blob, inflated in a version 2 pack
    if (g_pak_ver != 2 && g_pak_ver != 4) { void *b = malloc(n ? n : 1); memcpy(b, p, n); *out = n; return b; }
    if (n < 4) return NULL;
    uint32_t raw; memcpy(&raw, p, 4); uLongf dl = raw; void *b = malloc(raw ? raw : 1);
    if (uncompress(b, &dl, p + 4, (uLong)(n - 4)) != Z_OK || dl != raw) { free(b); return NULL; }
    *out = raw; return b;
}
static int pak_get(const uint8_t key[8], uint32_t **spv, size_t *ns, char **vkr, size_t *nr, uint32_t **glspv, size_t *ngl) {
    if (!pak_open()) return 0;
    *glspv = NULL; *ngl = 0;
    size_t entry_size = g_pak_ver >= 3 ? 44 : 32;
    uint32_t lo = 0, hi = g_pak_count;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; const uint8_t *e = g_pak + 28 + entry_size * (size_t)m; int c = memcmp(e, key, 8);
        if (!c) { uint64_t so, ro; uint32_t sl, rl; memcpy(&so, e + 8, 8); memcpy(&sl, e + 16, 4); memcpy(&ro, e + 20, 8); memcpy(&rl, e + 28, 4);
                  if (so > g_pak_n || sl > g_pak_n - so || ro > g_pak_n || rl > g_pak_n - ro) return 0;
                  *spv = pak_blob(g_pak + so, sl, ns); *vkr = pak_blob(g_pak + ro, rl, nr);
                  if (!*spv || !*vkr) { free(*spv); free(*vkr); *spv = NULL; *vkr = NULL; return 0; }
                  if (g_pak_ver >= 3) { uint64_t go; uint32_t gl; memcpy(&go, e + 32, 8); memcpy(&gl, e + 40, 4);
                      if (gl) { if (go > g_pak_n || gl > g_pak_n - go || !(*glspv = pak_blob(g_pak + go, gl, ngl))) { free(*spv); free(*vkr); *spv = NULL; *vkr = NULL; return 0; } } }
                  return 1; }
        if (c < 0) lo = m + 1; else hi = m; }
    return 0;
}
static int pak_has(const uint8_t key[8]) {
    if (!pak_open()) return 0;
    size_t entry_size = g_pak_ver >= 3 ? 44 : 32;
    uint32_t lo = 0, hi = g_pak_count;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; int c = memcmp(g_pak + 28 + entry_size * (size_t)m, key, 8); if (!c) return 1; if (c < 0) lo = m + 1; else hi = m; }
    return 0;
}
// Shaders the game builds in memory match no game file, so the player's own setup cannot translate them from the dump (Shield: 45, profile shader_nvn_capture):
// the first time one is handed over and the pack lacks it, its 0x880-byte control section and its code area are saved for the setup's next run to translate
void mtl_shader_seen(const uint8_t key[8], const void *control, const void *code, uint32_t ncode) {
    static const char *dir; static int init; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static nvn_hmap seen = {.klen = 8};
    if (!init) { dir = getenv("DM_NVN_CAPTURE"); if (dir) mkdir(dir, 0755); init = 1; }
    if (!dir || !control || !code || !ncode || ncode >= (1u << 24)) return;
    pthread_mutex_lock(&mu);
    if (!nvn_hm_get(&seen, key)) {
        nvn_hm_put(&seen, key, (void *)1);
        shmap_load();
        if (nvn_hm_get(&g_shmap, key) && !pak_has(key)) {
            char hex[17], p[1200]; for (int i = 0; i < 8; i++) sprintf(hex + 2 * i, "%02x", key[i]);
            struct stat st; snprintf(p, sizeof p, "%s/%s.code", dir, hex);
            if (stat(p, &st)) {
                FILE *f; snprintf(p, sizeof p, "%s/%s.ctl", dir, hex);
                if ((f = fopen(p, "wb"))) { fwrite(control, 1, 0x880, f); fclose(f); }
                snprintf(p, sizeof p, "%s/%s.code.tmp", dir, hex);
                if ((f = fopen(p, "wb"))) { fwrite(code, 1, ncode, f); fclose(f); char q[1200]; snprintf(q, sizeof q, "%s/%s.code", dir, hex); rename(p, q); }
                VLOG("shader %s captured for translation (%s)", hex, dir);
            }
        }
    }
    pthread_mutex_unlock(&mu);
}
static vshader *shader_load(const uint8_t key[8], int st) {
    char hex[17]; for (int i = 0; i < 8; i++) sprintf(hex + 2 * i, "%02x", key[i]);
    char p[1200]; size_t ns = 0, nr = 0, ngl = 0; uint32_t *code = NULL, *glcode = NULL; char *meta = NULL;
    if (pak_get(key, &code, &ns, &meta, &nr, &glcode, &ngl)) {}
    else {
        snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, hex); code = file_read(p, &ns);
        snprintf(p, sizeof p, "%s/%s.vkr", g_spv_dir, hex); meta = file_read(p, &nr);
        if (st == 0) { snprintf(p, sizeof p, "%s/%s.gl.spv", g_spv_dir, hex); glcode = file_read(p, &ngl); }
    }
    if (!code || !meta) { VLOG("[RED] shader %s: %s missing", hex, !code ? "SPIR-V" : "reflection (.vkr)"); free(code); free(meta); free(glcode); return NULL; }
    meta = realloc(meta, nr + 1); meta[nr] = 0;
    vshader *s = calloc(1, sizeof *s); strcpy(s->key, hex); s->stage = st; s->shift = stage_shift(st);
    int spst; uint32_t ls[3];
    if (spv_reflect(code, ns / 4, s->res, 64, &s->nres, s->in, 32, &s->nin, &spst, ls)) { VLOG("[RED] shader %s: SPIR-V not readable", hex); free(code); free(meta); free(glcode); free(s); return NULL; }
    static const VkShaderStageFlagBits VS[6] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT };
    s->vkstage = spst >= 0 && spst < 6 ? VS[spst] : VK_SHADER_STAGE_VERTEX_BIT;
    // reflection: bindings by name (the combined image takes its texture slot from "<name>" and its sampler from "<name>Smplr"), c1, driver segment reads
    struct { char name[48], kind[16]; int reg, off, bank, rw, n; } b[96]; int nb = 0; uint8_t *c1 = NULL; uint32_t c1n = 0; int unfill = 0;
    for (char *ln = strtok(meta, "\n"); ln; ln = strtok(NULL, "\n")) {
        if (!strncmp(ln, "b ", 2) && nb < 96) { if (sscanf(ln + 2, "%47s %15s %d %d %d %d %d", b[nb].name, b[nb].kind, &b[nb].reg, &b[nb].off, &b[nb].bank, &b[nb].rw, &b[nb].n) == 7) nb++; }
        else if (!strncmp(ln, "c1 ", 3)) { const char *h = ln + 3; size_t l = strlen(h) / 2; c1 = malloc(l ? l : 1); for (size_t i = 0; i < l; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); c1[i] = (uint8_t)v; } c1n = (uint32_t)l; }
        else if (!strncmp(ln, "drv ", 4)) { int a, c; if (sscanf(ln + 4, "%d %d", &a, &c) == 2) { s->drv_read[0] = (uint8_t)a; s->drv_read[2] = (uint8_t)c; } }
        else if (!strncmp(ln, "colors ", 7)) { int m, k; if (sscanf(ln + 7, "%d %d", &m, &k) == 2) { s->color_mask = (uint8_t)m; s->color_known = (uint8_t)k; } }
        else if (!strncmp(ln, "unfill ", 7)) unfill = 1;
        else if (!strncmp(ln, "tf ", 3)) { int bb, l, nc, sd; if (sscanf(ln + 3, "%d %d %d %d", &bb, &l, &nc, &sd) == 4 && bb >= 0 && bb < 4 && nc >= 1 && nc <= 4) { s->tf[bb][0] = l; s->tf[bb][1] = nc; s->tf_stride[bb] = (uint16_t)sd; } }
    }
    if (unfill) VLOG("[RED] shader %s reads driver-segment locations the replay cannot fill", hex);
    for (int i = 0; i < s->nres; i++) {
        spv_res *r = &s->res[i]; vres_how *h = &s->how[i];
        if (!strcmp(r->name, "support_buffer")) { h->what = W_SUPPORT; continue; }
        int j = -1; for (int k = 0; k < nb; k++) if (!strcmp(b[k].name, r->name)) { j = k; break; }
        if (j < 0) { RED_ONCE(17, "shader %s: binding %s has no reflection (first case)", hex, r->name); continue; }
        const char *k = b[j].kind; h->reg = (uint16_t)(b[j].reg < 0 ? 0 : b[j].reg); h->off = (uint16_t)b[j].off; h->rw = (uint8_t)b[j].rw;
        if (!strcmp(k, "cbuf")) h->what = W_CBUF;
        else if (!strcmp(k, "c1")) h->what = W_C1;
        else if (!strcmp(k, "drvcb")) { h->what = b[j].bank == 2 ? W_DRV2 : W_DRV0; s->drv_read[b[j].bank == 2 ? 2 : 0] = 1; }
        else if (!strcmp(k, "sbuf")) h->what = W_SSBO;
        else if (!strcmp(k, "ubo_ptr")) h->what = W_UBOP;
        else if (!strcmp(k, "sbuf_ubo")) h->what = W_SBUFU;
        else if (!strcmp(k, "img")) h->what = W_IMG;
        else if (!strcmp(k, "tbuf")) h->what = W_TBUF;
        else if (!strcmp(k, "tex") || !strcmp(k, "tex_ubo")) {
            h->what = !strcmp(k, "tex") ? W_TEX : W_TEXU;
            char sn[64]; snprintf(sn, sizeof sn, "%sSmplr", r->name);
            for (int q = 0; q < nb; q++) if (!strcmp(b[q].name, sn)) {
                const char *sk = b[q].kind; h->sreg = (uint16_t)(b[q].reg < 0 ? 0 : b[q].reg); h->soff = (uint16_t)b[q].off;
                h->skind = !strcmp(sk, "samp_tex") ? S_TEX : !strcmp(sk, "samp") ? S_SEP : !strcmp(sk, "samp_ubo") ? S_UBO : !strcmp(sk, "samp_id") ? S_ID : S_NONE;
            }
            if (!h->skind) h->skind = S_TEX;
        }
        else RED_ONCE(18, "shader %s: binding kind %s not implemented (%s)", hex, k, r->name);
    }
    if (c1n || 1) c1_store(s, c1 ? c1 : (const uint8_t *)"", c1n);
    free(c1); free(meta);
    if (s->shift) spv_rebind(code, ns / 4, s->shift);
    ns = spv_strip_unused(code, ns / 4) * 4;   // capabilities declared but unused (Float64, fragment interlock): strict drivers refuse or mis-handle them
    { size_t nw; uint32_t *c2 = spv_image_caps(code, ns / 4, &nw); if (c2) { free(code); code = c2; ns = nw * 4; } }   // and the format-less storage image ones they need
    if (spst == 0 || spst == 2) { size_t nw; uint32_t *c2 = spv_init_outputs(code, ns / 4, &nw); if (c2) { free(code); code = c2; ns = nw * 4; } }   // outputs start at (0, 0, 0, 1) as on the hardware
    if (st == 0 && glcode) {
        size_t nw;
        if (s->shift) spv_rebind(glcode, ngl / 4, s->shift);
        ngl = spv_strip_unused(glcode, ngl / 4) * 4;
        uint32_t *c2 = spv_image_caps(glcode, ngl / 4, &nw); if (c2) { free(glcode); glcode = c2; ngl = nw * 4; }
        c2 = spv_init_outputs(glcode, ngl / 4, &nw); if (c2) { free(glcode); glcode = c2; ngl = nw * 4; }
        VkShaderModuleCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = ngl, .pCode = glcode };
        VkResult gr = vkCreateShaderModule(g_dev, &gci, NULL, &s->glmod);
        if (gr) { VLOG("[RED] shader %s: GL depth fallback module failed %d", hex, gr); s->glmod = VK_NULL_HANDLE; }
        free(glcode);
    } else free(glcode);
    if (st == 1) spv_frag_outputs(code, ns / 4, s->out_cls);
    spv_io_masks(code, ns / 4, &s->in_mask, &s->out_mask);
    if (st == 0 && (s->tf_stride[0] | s->tf_stride[1] | s->tf_stride[2] | s->tf_stride[3])) {   // transform feedback: the capture variant (tf_draw)
        size_t tw; uint32_t *tc = spv_tf_variant(code, ns / 4, (const int (*)[2])s->tf, TF_BIND, &tw);
        if (tc) { VkShaderModuleCreateInfo tci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = tw * 4, .pCode = tc }; if (vkCreateShaderModule(g_dev, &tci, NULL, &s->tfmod)) s->tfmod = VK_NULL_HANDLE; free(tc); }
        if (!s->tfmod) VLOG("[RED] shader %s: transform feedback capture variant failed", hex);
    }
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = ns, .pCode = code };
    VkResult r = vkCreateShaderModule(g_dev, &mci, NULL, &s->mod);
    if (st == 1) { s->code = code; s->nw = ns / 4; } else free(code);
    if (r) { VLOG("[RED] shader %s: vkCreateShaderModule failed %d", hex, r); if (s->glmod) vkDestroyShaderModule(g_dev, s->glmod, NULL); free(s); return NULL; }
    return s;
}
static pthread_mutex_t g_shmu = PTHREAD_MUTEX_INITIALIZER;   // the shader cache: the game's thread and the prewarm thread
static vshader *shader_get(const uint8_t key[8], int st) {
    pthread_mutex_lock(&g_shmu);
    vshader *s = nvn_hm_get(&g_shcache, key);
    if (!s) { s = shader_load(key, st); if (s) nvn_hm_put(&g_shcache, key, s); }
    pthread_mutex_unlock(&g_shmu);
    return s;
}
typedef struct vprog { vshader *sh[6]; VkDescriptorSetLayout dsl[2]; VkPipelineLayout pl[2]; uint8_t np[2]; } vprog;   // [0] graphics stages, [1] compute; np: descriptor sets instead of pushes
static nvn_hmap g_progs = {.klen = 8};   // program serial → vprog
static vprog *prog_get(const prog_t *p) {
    vprog *o = p->serial ? nvn_hm_get(&g_progs, &p->serial) : NULL; if (o) return o;
    shmap_load();
    o = calloc(1, sizeof *o);
    for (unsigned i = 0; i < p->nstage; i++) {
        uint8_t k[8]; if (!prog_key(p, i, k)) continue;
        intptr_t st1 = (intptr_t)nvn_hm_get(&g_shmap, k);
        if (!st1) { char hex[17]; for (int q = 0; q < 8; q++) sprintf(hex + 2 * q, "%02x", k[q]); VLOG("[RED] shader %s is not in the shader map", hex); continue; }
        vshader *s = shader_get(k, (int)st1 - 1); if (s) o->sh[st1 - 1] = s;
    }
    if (p->serial) nvn_hm_put(&g_progs, &p->serial, o);
    return o;
}
static VkDescriptorType desc_type(int kind) {
    switch (kind) { case SPV_RES_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; case SPV_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; case SPV_RES_TEX: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case SPV_RES_TBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; case SPV_RES_IMG: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; case SPV_RES_IBUF: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case SPV_RES_SAMP: return VK_DESCRIPTOR_TYPE_SAMPLER; case SPV_RES_SIMG: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
static VkPipelineLayout prog_layout(vprog *o, int compute) {
    if (o->pl[compute]) return o->pl[compute];
    VkDescriptorSetLayoutBinding b[324]; uint32_t n = 0;
    for (int st = compute ? 5 : 0; st < (compute ? 6 : 5); st++) { vshader *s = o->sh[st]; if (!s) continue;
        for (int i = 0; i < s->nres && n < 320; i++) b[n++] = (VkDescriptorSetLayoutBinding){ s->res[i].binding + s->shift, desc_type(s->res[i].kind), s->res[i].count, s->vkstage, NULL }; }
    int tf = !compute && o->sh[0] && o->sh[0]->tfmod;   // transform feedback: the capture buffers and { first, base[4] } (tf_draw)
    if (tf) for (int k = 0; k < 4; k++) if (o->sh[0]->tf_stride[k]) b[n++] = (VkDescriptorSetLayoutBinding){ TF_BIND + (uint32_t)k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 20 };
    // more descriptors than one push takes (Pokémon Legends: Arceus' terrain: 29, past the 32 of some Adreno drivers with other programs): this program uses descriptor sets
    uint32_t nd = 0; for (uint32_t i = 0; i < n; i++) nd += b[i].descriptorCount;
    o->np[compute] = (uint8_t)(g_nopush || nd > g_maxpush);
    if (o->np[compute] && !g_nopush) { static int said; if (!said++) VLOG("a program with %u descriptors (push limit %u): descriptor sets for such programs", nd, g_maxpush); }
    VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = o->np[compute] ? 0 : VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = n, .pBindings = b };
    VKCHECK(vkCreateDescriptorSetLayout(g_dev, &dci, NULL, &o->dsl[compute]));
    VkPipelineLayoutCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &o->dsl[compute], .pushConstantRangeCount = tf ? 1u : 0u, .pPushConstantRanges = &pcr };
    VKCHECK(vkCreatePipelineLayout(g_dev, &pci, NULL, &o->pl[compute]));
    return o->pl[compute];
}

// ================= Replay state (as nvn_mtl.m) =================
static struct { p_set_rt rt; int has_rt; uint8_t clear_c[8]; VkClearColorValue cc[8]; uint8_t clear_d, clear_s; float cd; uint32_t cs; p_scissor sc; int has_sc; } R;
static struct {
    const prog_t *prog, *cprog, *bst[5];
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16]; uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; int depth_clamp; float blend_color[4]; int64_t copy_stride, copy_img_stride; int rast_discard; p_tf_bind tf[4]; uint64_t tf_off[4]; int tf_on;
} S;
static _Atomic int g_origin_ll, g_depth_zero;
void mtl_set_origin_lower_left(int on) { if (atomic_exchange(&g_origin_ll, on) != on) VLOG("window origin: %s", on ? "lower left (GL)" : "upper left"); }
void mtl_set_depth_zero(int on) { if (atomic_exchange(&g_depth_zero, on) != on) VLOG("depth mode: %s", on ? "near = 0" : "near = -w (GL)"); }
static void state_reset(void) {
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}
// NVN binds programs per stage (nvn_mtl.m bind_stages): stages of a program bound for only some of them combine into a host program
static const prog_t *bind_stages(const prog_t *p, int stages) {
    for (int st = 0; st < 5; st++) if (stages >> st & 1) S.bst[st] = p;
    const prog_t *v = S.bst[0]; if (!v) return NULL;
    int same = 1; for (int st = 1; st < 5; st++) if (S.bst[st] && S.bst[st] != v) same = 0;
    if (same && S.bst[1] == v) return v;
    // The combination by the shaders it ends up with, made once and kept: a 64-entry ring of recent combinations was here, and Shield cycles through more than 64
    // a frame, so nearly every draw missed and made a new host program with two new Vulkan layouts that were never freed (3.5 million in 90 s on the Mac: the memory
    // creep, and driver work on every draw). The set of shader combinations is the game's, so this map stops growing once they have all been seen
    static nvn_hmap comb = {.klen = sizeof(vshader *) * 5};
    vshader *key[5] = {0};
    for (int st = 0; st < 5; st++) if (S.bst[st]) { vprog *src = prog_get(S.bst[st]); key[st] = src ? src->sh[st] : NULL; }
    prog_t *cp = nvn_hm_get(&comb, key); if (cp) return cp;
    vprog *o = calloc(1, sizeof *o); memcpy(o->sh, key, sizeof key);
    cp = calloc(1, sizeof *cp); cp->magic = MAGIC('P', 'R', 'O', 'G'); cp->serial = nvn_new_serial();
    nvn_hm_put(&g_progs, &cp->serial, o); nvn_hm_put(&comb, key, cp);
    return cp;
}
void mtl_prog_release(prog_t *p) { (void)p; }   // programs are kept (keyed by serial; a released one is never looked up again)

// ================= Render passes =================
static struct { int open; uint32_t cfmt[8], dfmt, sfmt, samples, tw, th; int hd, hs, ncolor; } P;
static VkCommandBuffer CB(void) { return g_sub->cb; }
static void pass_end(void) { if (P.open) { vkCmdEndRendering(CB()); P.open = 0; } }
static int rt_dup_slot(const p_set_rt *r, int i) {
    if (!r->color[i]) return 0;
    uint8_t lv = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0; uint16_t ly = r->has_cview[i] && r->cview[i].set_layers ? r->cview[i].min_layer : 0;
    for (int j = 0; j < i; j++) { if (!r->color[j] || r->color[j]->serial != r->color[i]->serial) continue;
        uint8_t lj = r->has_cview[j] && r->cview[j].set_levels ? r->cview[j].base_level : 0; uint16_t yj = r->has_cview[j] && r->cview[j].set_layers ? r->cview[j].min_layer : 0;
        if (lj == lv && yj == ly) return 1; }
    return 0;
}
// the attachment view of a render target: one level, one layer (3D: one depth slice through a 2D-array view is not available, the slice is taken as layer 0 — first case reported)
static VkImageView att_view(const tex_t *t, const view_t *v, int has_v, vtex **out, uint32_t *w, uint32_t *h) {
    vtex *x = tex_res(t); *out = x; if (!x || !x->img) return VK_NULL_HANDLE;
    if (!(x->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) { RED_ONCE(19, "render target ser=%llu format %#x cannot be attached (Vulkan format %d)", (unsigned long long)t->serial, t->d.format, x->fmt); return VK_NULL_HANDLE; }
    uint32_t lv = has_v && v->set_levels ? v->base_level : 0, ly = has_v && v->set_layers ? v->min_layer : 0;
    if (x->is3d) { if (ly) RED_ONCE(20, "render target is a slice of a 3D texture: not supported yet"); ly = 0; }
    if (lv >= x->levels || ly >= x->layers) return VK_NULL_HANDLE;
    if (x->cube_ok) { static uint64_t said[32]; static int nsaid; int seen = 0; uint64_t key = t->serial << 8 | ly << 4 | lv;
        for (int q = 0; q < nsaid; q++) if (said[q] == key) seen = 1;
        if (!seen && nsaid < 32) { said[nsaid++] = key; VLOG("render target: cube texture ser %llu level %u layer %u, view layers %u (set %d)", (unsigned long long)t->serial, lv, ly,
            has_v && v->set_layers ? v->nlayers : 0, has_v && v->set_layers); } }
    VkFormat f = x->fmt;
    if (has_v && v->set_format) { uint32_t vf = 0; uint8_t s[4]; if (nvn_fmt_to_vk(v->format, &vf, s) && vf) f = (VkFormat)vf; }
    *w = x->w >> lv ? x->w >> lv : 1; *h = x->h >> lv ? x->h >> lv : 1;
    return view_get(x, x->is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, f, lv, 1, ly, 1, SW_ID, x->aspect);
}
// DM_NVN_TEXDUMP=frame:path (as nvn_mtl.m): every colour render target that frame uses, saved once the frame is over as <path with %d = index * 100 + level> (PNG, clamped to [0,1])
static long g_td_frame = -2; static const char *g_td_path; static int g_td_done;
static struct { vtex *x; uint64_t ser; uint32_t lv, sl; } g_td[256]; static int g_ntd;
static void shot_level(vtex *x, uint32_t lv, uint32_t sl, const char *path);
static void td_note(vtex *x, uint64_t ser, uint32_t lv, uint32_t sl) {
    if (g_td_frame == -2) { const char *e = getenv("DM_NVN_TEXDUMP"), *c = e ? strchr(e, ':') : NULL; g_td_frame = c ? atol(e) : -1; g_td_path = c ? c + 1 : NULL; }
    if (g_td_frame < 0 || g_td_done) return;
    if ((long)g_frame_no > g_td_frame) {
        g_td_done = 1; pass_end(); uint64_t f = mtl_sub_commit(); mtl_wait(f, UINT64_MAX); mtl_sub_begin();
        for (int i = 0; i < g_ntd; i++) { char path[1024]; snprintf(path, sizeof path, g_td_path, (int)(g_td[i].ser * 100 + g_td[i].sl * 10 + g_td[i].lv)); shot_level(g_td[i].x, g_td[i].lv, g_td[i].sl, path);
            VLOG("[texdump] %d ser=%llu %ux%u level %u format %d → %s", i, (unsigned long long)g_td[i].ser, g_td[i].x->w, g_td[i].x->h, g_td[i].lv, g_td[i].x->fmt, path); }
        return;
    }
    if ((long)g_frame_no != g_td_frame || g_ntd >= 256) return;
    for (int i = 0; i < g_ntd; i++) if (g_td[i].ser == ser && g_td[i].lv == lv && g_td[i].sl == sl) return;
    vtex_ref(x); g_td[g_ntd].x = x; g_td[g_ntd].ser = ser; g_td[g_ntd].lv = lv; g_td[g_ntd].sl = sl; g_ntd++;
}
// A transform feedback capture runs in a 1x1 pass of its own with no attachments (as nvn_mtl.m tf_draw's encoder): its pipeline has rasterizer discard on, so it has no colour or
// depth state, and inside the game's pass its attachment count would not match the pass (VUID-vkCmdDraw-colorAttachmentCount-06179). Ended by pass_end like any pass
static void tf_pass_begin(void) {
    pass_end(); memset(&P, 0, sizeof P);
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {1, 1}}, .layerCount = 1 };
    vkCmdBeginRendering(CB(), &ri); P.open = 1; P.tw = P.th = 1; P.samples = 1;
}
static uint64_t g_stat_begins;   // vkCmdBeginRendering calls (the present line's statistics)
static int pass_begin(void) {
    if (P.open) return 1;
    if (!R.has_rt) { RED_ONCE(21, "draw with no render target set"); return 0; }
    VkRenderingAttachmentInfo ca[8]; VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, sa = da;
    memset(&P, 0, sizeof P); uint32_t tw = 16384, th = 16384; int any = 0, ncol = 0;
    for (int i = 0; i < 8; i++) { ca[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }; }
    for (int i = 0; i < R.rt.n && i < 8; i++) {
        if (!R.rt.color[i] || rt_dup_slot(&R.rt, i)) continue;
        vtex *x; uint32_t w, h; VkImageView v = att_view(R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i], &x, &w, &h);
        if (!v || !(x->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) continue;
        ca[i].imageView = v; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[i].loadOp = R.clear_c[i] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].clearValue.color = R.cc[i];
        VkFormat f = x->fmt; if (R.rt.has_cview[i] && R.rt.cview[i].set_format) { uint32_t vf = 0; uint8_t s[4]; if (nvn_fmt_to_vk(R.rt.cview[i].format, &vf, s) && vf) f = (VkFormat)vf; }
        P.cfmt[i] = (uint32_t)f; P.samples = x->samples; if (w < tw) tw = w; if (h < th) th = h; any = 1; ncol = i + 1;
        td_note(x, R.rt.color[i]->serial, R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0, R.rt.has_cview[i] && R.rt.cview[i].set_layers ? R.rt.cview[i].min_layer : 0);
    }
    if (R.rt.depth) {
        vtex *x; uint32_t w, h; VkImageView v = att_view(R.rt.depth, &R.rt.dview, R.rt.has_dview, &x, &w, &h);
        if (v && (x->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            if (x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da.imageView = v; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                da.loadOp = R.clear_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; da.clearValue.depthStencil.depth = R.cd; P.hd = 1; P.dfmt = (uint32_t)x->fmt; }
            if (x->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.imageView = v; sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                sa.loadOp = R.clear_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; sa.clearValue.depthStencil.stencil = R.cs; P.hs = 1; P.sfmt = (uint32_t)x->fmt; }
            P.samples = x->samples; if (w < tw) tw = w; if (h < th) th = h; any = 1;
        }
    }
    if (!any) {   // no target attached: an attachment-less pass at the viewport's size (nvn_mtl.m bare_dims), or nothing when targets were set but none could be attached
        int bare = !R.rt.depth; for (int i = 0; i < R.rt.n; i++) if (R.rt.color[i]) bare = 0;
        if (!bare) { RED_ONCE(22, "no render target could be attached"); return 0; }
        double x1 = S.has_vp ? fmax(S.vp[0][0], S.vp[0][0] + S.vp[0][2]) : R.has_sc ? (double)R.sc.x + R.sc.w : 1, y1 = S.has_vp ? fmax(S.vp[0][1], S.vp[0][1] + S.vp[0][3]) : R.has_sc ? (double)R.sc.y + R.sc.h : 1;
        tw = (uint32_t)fmin(fmax(ceil(x1), 1), 16384); th = (uint32_t)fmin(fmax(ceil(y1), 1), 16384); P.samples = 1;
    }
    if (!P.samples) P.samples = 1;
    P.tw = tw; P.th = th; P.ncolor = ncol;
    barrier_all(CB());
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {tw, th}}, .layerCount = 1, .colorAttachmentCount = (uint32_t)ncol, .pColorAttachments = ca,
                           .pDepthAttachment = P.hd ? &da : NULL, .pStencilAttachment = P.hs ? &sa : NULL };
    vkCmdBeginRendering(CB(), &ri); g_stat_begins++;
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    P.open = 1;
    return 1;
}
static int getenv_nomerge(void) { static int v = -1; if (v < 0) v = getenv("DM_VK_NO_PASS_MERGE") != NULL; return v; }   // A/B switch
static void pass_flush(void) {   // pending clears with no draw after them: a pass just for them
    int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
    if (pend && !P.open && g_sub) pass_begin();
    pass_end();
}
static void outside_pass(void) { pass_end(); barrier_all(CB()); }   // transfer work: no pass open, ordered after everything before

// ================= Pipelines =================
typedef struct {
    void *sh[5]; uint32_t cfmt[8], dfmt, sfmt; uint32_t chmask; blend_st blend[8];
    struct { uint32_t fmt, off; uint8_t bind, used, inst, pad; uint32_t div; } attr[32];
    uint8_t samples, a2c, poly, clamp, topo, logic_en, logic_op, blend_en, depth_zero, discard, pad[6];
} pkey;
static nvn_hmap g_pso = {.klen = sizeof(pkey)}, g_pso_fail = {.klen = sizeof(pkey)};
static VkPipelineCache g_pcache;
static pthread_mutex_t g_psomu = PTHREAD_MUTEX_INITIALIZER;   // g_pso / g_pso_fail, shader variants, the record file: the game's thread and the prewarm thread
static atomic_int g_pso_new;                                  // pipelines built since the cache file was last written
static atomic_int g_pso_ingame;                               // pipelines the game's draws had to build (a prewarm miss: a possible hitch)
static VkPrimitiveTopology topo_of(int prim) {
    switch (prim) { case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
}
static int topo_class(VkPrimitiveTopology t) { return t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 0 : t <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? 1 : 2; }
static VkBlendFactor bfac(int f) {
    switch (f) { case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp bop(int e) { switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; } return VK_BLEND_OP_ADD; }
// vertex format for a shader input of class ct (0 float, 1 uint, 2 sint): an integer format of the other signedness is read with the same width and the input's sign
static VkFormat vfmt_for(int nvn_fmt, int ct) {
    int fc; VkFormat f = (VkFormat)nvn_vfmt_to_vk(nvn_fmt, &fc);
    if (!f) return VK_FORMAT_UNDEFINED;
    if (ct == fc || (ct == 0 && fc == 0)) return f;
    static const VkFormat U[] = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT };
    static const VkFormat I[] = { VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT };
    static const VkFormat F32[] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    for (int i = 0; i < 12; i++) {
        if (U[i] == f || I[i] == f) { if (ct == 1) return U[i]; if (ct == 2) return I[i]; if (i >= 8) return F32[i - 8]; RED_ONCE(23, "vertex format %#x (8/16-bit integer) feeds a float input", nvn_fmt); return f; }
    }
    // a float-type format into an integer input: the same bytes read as integers of the same layout (Maxwell gives the converted float's bits; noted once)
    RED_ONCE(24, "vertex format %#x feeds an integer input: raw bytes read as integers", nvn_fmt);
    if (f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT) { int n = f == VK_FORMAT_R32_SFLOAT ? 0 : f == VK_FORMAT_R32G32_SFLOAT ? 1 : f == VK_FORMAT_R32G32B32_SFLOAT ? 2 : 3; return ct == 1 ? U[8 + n] : I[8 + n]; }
    return f;
}
// The fragment module for a pipeline: on the Switch a varying the previous stage never writes reads undefined values; Vulkan (and Metal under MoltenVK) refuse the pipeline,
// so such inputs become private variables holding the hardware's (0, 0, 0, 1) in a variant of the fragment shader (made once per shader and set of missing locations)
// The fragment module for a pipeline. On the Switch a varying the previous stage never writes reads (0, 0, 0, 1) and a float output to an integer colour target is written
// as its bits; Vulkan (and Metal under MoltenVK) refuse both, so such a pipeline gets a variant of the fragment shader (made once per shader and combination)
static VkShaderModule frag_module(vprog *pr, const uint32_t *cfmt) {
    vshader *fs = pr->sh[1], *pv = pr->sh[2] ? pr->sh[2] : pr->sh[4] ? pr->sh[4] : pr->sh[0];
    uint64_t drop = fs->in_mask & ~(pv ? pv->out_mask : 0); uint32_t um = 0, sm = 0;
    for (int i = 0; i < 8; i++) if (cfmt[i] && fs->out_cls[i] == 0) { int c = fmt_int_class((VkFormat)cfmt[i]); if (c == 1) um |= 1u << i; else if (c == 2) sm |= 1u << i; }
    if ((!drop && !um && !sm) || !fs->code) return fs->mod;
    pthread_mutex_lock(&g_psomu);
    for (int i = 0; i < fs->nvar; i++) if (fs->var[i].drop == drop && fs->var[i].um == um && fs->var[i].sm == sm) { VkShaderModule m = fs->var[i].mod; pthread_mutex_unlock(&g_psomu); return m; }
    pthread_mutex_unlock(&g_psomu);
    size_t nw = fs->nw; uint32_t *c = NULL, *c2;
    if (drop && (c2 = spv_drop_inputs(fs->code, fs->nw, drop, &nw))) c = c2;
    if ((um || sm) && (c2 = spv_int_outputs(c ? c : fs->code, c ? nw : fs->nw, um, sm, &nw))) { free(c); c = c2; }
    if (!c) return fs->mod;
    VkShaderModule m = VK_NULL_HANDLE; VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = c };
    VkResult r = vkCreateShaderModule(g_dev, &mci, NULL, &m); free(c);
    if (r) { VLOG("[RED] fragment shader %s: variant failed %d", fs->key, r); return fs->mod; }
    if (drop) VLOG("fragment shader %s reads varyings %#llx that %s never writes: those read (0, 0, 0, 1)", fs->key, (unsigned long long)drop, pv ? pv->key : "-");
    if (um | sm) VLOG("fragment shader %s: float outputs %#x / %#x written as uint / int bits (integer colour targets)", fs->key, um, sm);
    pthread_mutex_lock(&g_psomu);
    if (fs->nvar < 8) { fs->var[fs->nvar].drop = drop; fs->var[fs->nvar].um = um; fs->var[fs->nvar].sm = sm; fs->var[fs->nvar].mod = m; fs->nvar++; }
    pthread_mutex_unlock(&g_psomu);
    return m;
}
// ---------- Pipeline list and cache (Android): every pipeline the game makes is recorded (DM_VK_PSO_RECORD) as its shaders' keys plus the rest of its key; at start the
// recorded list and the list shipped with the game (DM_VK_PSO_SEED) are built on a background thread before the game asks for them, and the driver's compiled pipelines are
// kept in a pipeline cache file (DM_VK_PCACHE), so a later start compiles nothing it already compiled
typedef struct { uint8_t key[5][8]; pkey k; } pso_rec;   // k.sh zeroed
#define PSO_MAGIC "NXPL"
static FILE *g_psorec;
static void pso_record(const vprog *pr, const pkey *k) {   // caller holds g_psomu
    static int tried; if (!tried++) { const char *e = getenv("DM_VK_PSO_RECORD"); if (e) { g_psorec = fopen(e, "ab"); if (g_psorec && ftell(g_psorec) == 0) { uint32_t v[2] = { 1, (uint32_t)sizeof(pso_rec) }; fwrite(PSO_MAGIC, 1, 4, g_psorec); fwrite(v, 4, 2, g_psorec); } } }
    if (!g_psorec) return;
    pso_rec r; memset(&r, 0, sizeof r); r.k = *k; memset(r.k.sh, 0, sizeof r.k.sh);
    for (int i = 0; i < 5; i++) if (pr->sh[i]) hexkey(pr->sh[i]->key, r.key[i]);
    fwrite(&r, sizeof r, 1, g_psorec); fflush(g_psorec);
}
static VkPipeline pso_make(vprog *pr, pkey k);
static VkPipeline pso_lookup(vprog *pr, const pkey *k, int record) {
    pthread_mutex_lock(&g_psomu);
    VkPipeline p = nvn_hm_get(&g_pso, k); int failed = p ? 0 : nvn_hm_get(&g_pso_fail, k) != NULL;
    pthread_mutex_unlock(&g_psomu);
    if (p || failed) return p;
    p = pso_make(pr, *k);   // compiled outside the lock: the prewarm thread and the game's thread compile in parallel
    pthread_mutex_lock(&g_psomu);
    VkPipeline q = nvn_hm_get(&g_pso, k);
    if (q) { pthread_mutex_unlock(&g_psomu); if (p) vkDestroyPipeline(g_dev, p, NULL); return q; }   // the other thread finished the same one first
    if (p) { nvn_hm_put(&g_pso, k, p); if (record) { pso_record(pr, k); atomic_fetch_add(&g_pso_ingame, 1); } atomic_fetch_add(&g_pso_new, 1); }
    else nvn_hm_put(&g_pso_fail, k, (void *)1);
    pthread_mutex_unlock(&g_psomu);
    return p;
}
static void pcache_save(void) {
    const char *path = getenv("DM_VK_PCACHE"); if (!path || !g_pcache) return;
    size_t n = 0; if (vkGetPipelineCacheData(g_dev, g_pcache, &n, NULL) || !n) return;
    void *d = malloc(n); if (vkGetPipelineCacheData(g_dev, g_pcache, &n, d)) { free(d); return; }
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "wb");
    if (f) { int ok = fwrite(d, 1, n, f) == n; ok &= !fclose(f); if (ok) rename(tmp, path); }
    free(d); atomic_store(&g_pso_new, 0);
    VLOG("pipeline cache saved: %.1f MB", n / 1048576.0);
}
void nvn_vk_cache_save(void) { if (g_dev) pcache_save(); }
void *saver(void *a) { pcache_save(); atomic_store((atomic_int *)a, 0); return NULL; }   // the app calls it when going to the background
static void pcache_open(void) {
    const char *path = getenv("DM_VK_PCACHE"); size_t n = 0; void *d = NULL;
    if (path) { FILE *f = fopen(path, "rb"); if (f) { fseek(f, 0, SEEK_END); long l = ftell(f); rewind(f); if (l > 0) { d = malloc((size_t)l); n = fread(d, 1, (size_t)l, f); } fclose(f); } }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = n, .pInitialData = d };
    if (vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache)) { ci.initialDataSize = 0; ci.pInitialData = NULL; vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache); n = 0; }   // a cache of another driver version: start empty
    free(d);
    if (path) VLOG("pipeline cache: %.1f MB from %s", n / 1048576.0, path);
}
static int pso_rec_load(const char *path, pso_rec **out, int *nout) {
    FILE *f = path ? fopen(path, "rb") : NULL; if (!f) return 0;
    char m[4]; uint32_t v[2];
    if (fread(m, 1, 4, f) != 4 || memcmp(m, PSO_MAGIC, 4) || fread(v, 4, 2, f) != 2 || v[0] != 1 || v[1] != sizeof(pso_rec)) { fclose(f); VLOG("[RED] pipeline list %s: not this version's format", path); return 0; }
    pso_rec r; int k = 0;
    while (fread(&r, sizeof r, 1, f) == 1) { if (*nout % 1024 == 0) *out = realloc(*out, sizeof **out * (size_t)(*nout + 1024)); (*out)[(*nout)++] = r; k++; }
    fclose(f); return k;
}
// The prewarm stays gentle on memory: one pipeline at a time with a short pause after each (the driver's compiler gets its memory back between them), and on Linux / Android
// it waits while the system has less than DM_VK_PREWARM_MIN_MB (1536) available, so it never pushes the phone into the low-memory killer while the game is loading
static long mem_avail_mb(void) {
#ifdef __linux__
    FILE *f = fopen("/proc/meminfo", "r"); char ln[128]; long v = -1;
    while (f && fgets(ln, sizeof ln, f)) if (!strncmp(ln, "MemAvailable:", 13)) { v = atol(ln + 13) / 1024; break; }
    if (f) fclose(f); return v;
#else
    return -1;
#endif
}
static void prewarm_pace(int i, int *waits) {
    usleep(3000);
    if (i % 8) return;
    static long floor_mb = -1; if (floor_mb < 0) { const char *e = getenv("DM_VK_PREWARM_MIN_MB"); floor_mb = e ? atol(e) : 1536; }
    for (int k = 0; k < 600; k++) { long m = mem_avail_mb(); if (m < 0 || m >= floor_mb) break; if (!k) (*waits)++; usleep(100000); }   // at most a minute per wait
}
static void *prewarm_main(void *arg) {
    (void)arg; uint64_t t0 = mono_ns(); int waits = 0;
    pso_rec *rs = NULL; int n = 0;
    int ns = pso_rec_load(getenv("DM_VK_PSO_SEED"), &rs, &n), nr = pso_rec_load(getenv("DM_VK_PSO_RECORD"), &rs, &n);
    if (!n) return NULL;
    static nvn_hmap progs = {.klen = 40}; static nvn_hmap seen = {.klen = sizeof(pso_rec)};
    int built = 0, had = 0, failed = 0, noshader = 0;
    for (int i = 0; i < n; i++) {
        if (nvn_hm_get(&seen, &rs[i])) continue; nvn_hm_put(&seen, &rs[i], (void *)1);
        vprog *pr = nvn_hm_get(&progs, rs[i].key);
        if (!pr) {
            pr = calloc(1, sizeof *pr); int ok = 1;
            for (int st = 0; st < 5; st++) { static const uint8_t z[8]; if (!memcmp(rs[i].key[st], z, 8)) continue; pr->sh[st] = shader_get(rs[i].key[st], st); if (!pr->sh[st]) ok = 0; }
            if (!ok || !pr->sh[0]) { free(pr); noshader++; continue; }
            nvn_hm_put(&progs, rs[i].key, pr);
        }
        pkey k = rs[i].k; for (int st = 0; st < 5; st++) k.sh[st] = pr->sh[st];
        prog_layout(pr, 0);
        pthread_mutex_lock(&g_psomu); int have = nvn_hm_get(&g_pso, &k) != NULL; pthread_mutex_unlock(&g_psomu);
        if (have) { had++; continue; }
        if (pso_lookup(pr, &k, 0)) built++; else failed++;
        prewarm_pace(built + failed, &waits);
    }
    VLOG("prewarm: %d pipelines listed (%d shipped, %d recorded here): %d built, %d already there, %d failed, %d with a shader missing, %.1f s (paused %d times for low memory)", n, ns, nr, built, had, failed, noshader, (mono_ns() - t0) / 1e9, waits);
    free(rs);
    pcache_save();
    return NULL;
}
// Builds the pipeline for key k (no lookup: pso_get and the prewarm do that)
static VkPipeline pso_make(vprog *pr, pkey k) {
    vshader *vs = pr->sh[0], *fs = pr->sh[1]; VkPipeline p = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[5]; uint32_t nst = 0;
    if (!g_clipctl && !k.depth_zero && vs && !vs->glmod) RED_ONCE(27, "GL depth mode without VK_EXT_depth_clip_control and no translated depth fallback for vertex shader %s", vs->key);
    for (int i = 0; i < 5; i++) if (pr->sh[i] && !(i == 1 && k.discard)) st[nst++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = pr->sh[i]->vkstage, .module = i == 1 ? frag_module(pr, k.cfmt) : i == 0 && k.discard && pr->sh[0]->tfmod ? pr->sh[0]->tfmod : i == 0 && !g_clipctl && !k.depth_zero && pr->sh[0]->glmod ? pr->sh[0]->glmod : pr->sh[i]->mod, .pName = "main" };
    VkVertexInputBindingDescription vb[17]; VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDivisorDescriptionEXT vd[16]; uint32_t nvb = 0, nva = 0, nvd = 0; uint32_t have = 0;
    for (uint32_t loc = 0; loc < 32; loc++) {
        if (!k.attr[loc].used) continue;
        va[nva++] = (VkVertexInputAttributeDescription){ loc, k.attr[loc].bind, (VkFormat)k.attr[loc].fmt, k.attr[loc].bind == 16 ? 0 : k.attr[loc].off };
        uint32_t b = k.attr[loc].bind; if (have >> b & 1) continue; have |= 1u << b;
        vb[nvb++] = (VkVertexInputBindingDescription){ b, 0, b != 16 && k.attr[loc].inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX };
        if (b != 16 && k.attr[loc].inst && k.attr[loc].div > 1) { if (g_divisor) vd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){ b, k.attr[loc].div }; else RED_ONCE(26, "instance divisor %u needs VK_EXT_vertex_attribute_divisor", k.attr[loc].div); }
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = vd };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nvd ? &vds : NULL, .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = vb, .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = k.topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k.topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, .negativeOneToOne = !k.depth_zero };   // NVN's GL depth mode: z in [-w, w]
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = g_clipctl ? &dcc : NULL, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k.clamp, .rasterizerDiscardEnable = k.discard,
        .polygonMode = k.poly == 1 ? VK_POLYGON_MODE_LINE : k.poly == 2 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL, .depthBiasEnable = VK_TRUE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k.samples ? k.samples : 1), .alphaToCoverageEnable = k.a2c };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cb[8]; uint32_t ncb = 0; VkFormat cf[8];
    for (int i = 0; i < 8; i++) { if (k.cfmt[i]) ncb = (uint32_t)i + 1; }
    for (uint32_t i = 0; i < ncb; i++) {
        cf[i] = (VkFormat)k.cfmt[i]; memset(&cb[i], 0, sizeof cb[i]);
        uint32_t m = (k.chmask >> (4 * i)) & 0xf;
        if (fs && fs->color_known && !(fs->color_mask >> i & 1)) m = 0;   // outputs the shader does not write keep their content (nvn_mtl.m pso_desc)
        cb[i].colorWriteMask = m;
        if (k.blend_en >> i & 1 && m && !fmt_int_class(cf[i])) { cb[i].blendEnable = VK_TRUE;   // integer targets never blend (Vulkan: undefined)
            cb[i].srcColorBlendFactor = bfac(k.blend[i].src_rgb); cb[i].dstColorBlendFactor = bfac(k.blend[i].dst_rgb); cb[i].srcAlphaBlendFactor = bfac(k.blend[i].src_a); cb[i].dstAlphaBlendFactor = bfac(k.blend[i].dst_a);
            cb[i].colorBlendOp = bop(k.blend[i].eq_rgb); cb[i].alphaBlendOp = bop(k.blend[i].eq_a); }
    }
    VkPipelineColorBlendStateCreateInfo bs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .logicOpEnable = k.logic_en, .logicOp = (VkLogicOp)k.logic_op,
                                           .attachmentCount = ncb, .pAttachments = cb };
    VkDynamicState dyn[24]; uint32_t nd = 0;
    VkDynamicState base[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP };
    for (unsigned i = 0; i < sizeof base / sizeof *base; i++) dyn[nd++] = base[i];
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = nd, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = ncb, .pColorAttachmentFormats = cf,
        .depthAttachmentFormat = (VkFormat)k.dfmt, .stencilAttachmentFormat = (VkFormat)k.sfmt };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = nst, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &bs, .pDynamicState = &dy, .layout = prog_layout(pr, 0) };
#ifdef __ANDROID__
    { static int npipe; VLOG("pipeline %d: vertex %s fragment %s", ++npipe, vs->key, fs ? fs->key : "-"); }   // the last line before a driver crash names the shaders it was compiling
#endif
    VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &gi, NULL, &p);
    if (r) { VLOG("[RED] graphics pipeline (vertex %s fragment %s) failed: %d", vs->key, fs ? fs->key : "-", r); return VK_NULL_HANDLE; }
    return p;
}
static VkPipeline pso_get(vprog *pr, VkPrimitiveTopology topo) {
    vshader *vs = pr->sh[0], *fs = pr->sh[1];
    pkey k; memset(&k, 0, sizeof k);
    for (int i = 0; i < 5; i++) k.sh[i] = pr->sh[i];
    memcpy(k.cfmt, P.cfmt, sizeof k.cfmt); k.dfmt = P.hd ? P.dfmt : 0; k.sfmt = P.hs ? P.sfmt : 0; k.samples = (uint8_t)P.samples;
    k.chmask = S.chmask.mask; k.blend_en = (uint8_t)S.color.blend_en; for (int i = 0; i < 8; i++) if (k.cfmt[i]) k.blend[i] = S.blend[i];
    k.a2c = S.ms.a2c; k.poly = S.poly.mode == NVN_POLYGON_MODE_LINE ? 1 : S.poly.mode == NVN_POLYGON_MODE_POINT ? 2 : 0; k.clamp = (uint8_t)!!S.depth_clamp; k.topo = (uint8_t)topo_class(topo);
    k.logic_en = S.color.logic_op != NVN_LOGIC_OP_COPY; k.logic_op = (uint8_t)S.color.logic_op; k.depth_zero = (uint8_t)atomic_load(&g_depth_zero); k.discard = (uint8_t)!!S.rast_discard;
    for (int i = 0; i < vs->nin; i++) {
        uint32_t loc = vs->in[i].location; if (loc >= 32) continue;
        k.attr[loc].used = 1;
        if ((int)loc < S.nattr && S.attr[loc].fmt) { int st = S.attr[loc].stream; VkFormat f = vfmt_for(S.attr[loc].fmt, vs->in[i].sclass);
            if (f && st < 16) { k.attr[loc].fmt = f; k.attr[loc].off = S.attr[loc].offset; k.attr[loc].bind = (uint8_t)st; k.attr[loc].inst = S.stream[st].divisor != 0; k.attr[loc].div = S.stream[st].divisor; continue; }
            RED_ONCE(25, "vertex attribute format %#x not mapped", S.attr[loc].fmt); }
        k.attr[loc].fmt = vs->in[i].sclass == 1 ? VK_FORMAT_R32G32B32A32_UINT : vs->in[i].sclass == 2 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT; k.attr[loc].bind = 16;   // unbound: reads 0
    }
    return pso_lookup(pr, &k, 1);
}

// ================= Descriptors =================
static uint32_t g_draw_binst, g_dispatch_dim[3];
static VkBuffer g_c0_buf; static VkDeviceSize g_c0_off;
static uint32_t ubo_word(int stage, int reg, int off) {   // a bindless handle the game keeps in a uniform buffer: read from the pool now (nvn_mtl.m ubo_read)
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16) return 0;
    const p_bind_buf *u = &S.ubo[stage][reg]; uint32_t h = 0;
    if (u->addr && (uint64_t)off + 4 <= u->size) memcpy(&h, (const void *)(uintptr_t)(u->addr + off), 4);
    return h;
}
// g_nopush: the writes of all stages of a draw, gathered here and written into one set (desc_flush)
static struct { VkWriteDescriptorSet w[320]; VkDescriptorBufferInfo bi[320]; VkDescriptorImageInfo ii[320]; VkBufferView bv[320]; uint32_t n; } D;
static VkDescriptorSet desc_set_alloc(VkDescriptorSetLayout dsl) {
    sub_t *s = g_sub;
    for (;;) {
        if (s->cdp == s->ndp) {
            VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048 }, { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 2048 },
                { VK_DESCRIPTOR_TYPE_SAMPLER, 2048 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048 } };
            VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2048, .poolSizeCount = 8, .pPoolSizes = ps };
            VkDescriptorPool p; VKCHECK(vkCreateDescriptorPool(g_dev, &pci, NULL, &p));
            s->dp = realloc(s->dp, sizeof *s->dp * (size_t)(s->ndp + 1)); s->dp[s->ndp++] = p;
        }
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s->dp[s->cdp], .descriptorSetCount = 1, .pSetLayouts = &dsl };
        VkDescriptorSet ds; VkResult r = vkAllocateDescriptorSets(g_dev, &ai, &ds);
        if (r == VK_SUCCESS) return ds;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) VDIE("vkAllocateDescriptorSets failed: %d", r);
        s->cdp++;   // full: the next pool (made above when there is none)
    }
}
static void desc_flush(VkPipelineBindPoint bp, VkPipelineLayout pl, VkDescriptorSetLayout dsl) {
    if (!g_np || !D.n) { D.n = 0; return; }
    VkDescriptorSet ds = desc_set_alloc(dsl);
    for (uint32_t i = 0; i < D.n; i++) D.w[i].dstSet = ds;
    vkUpdateDescriptorSets(g_dev, D.n, D.w, 0, NULL);
    vkCmdBindDescriptorSets(CB(), bp, pl, 0, 1, &ds, 0, NULL);
    D.n = 0;
}
static void push_stage(vshader *s, int st, VkPipelineBindPoint bp, VkPipelineLayout pl) {
    if (!s || !s->nres) return;
    VkWriteDescriptorSet w_[64]; VkDescriptorBufferInfo bi_[64]; VkDescriptorImageInfo ii_[64]; VkBufferView bv_[64]; uint32_t nw = 0;
    VkWriteDescriptorSet *w = w_; VkDescriptorBufferInfo *bi = bi_; VkDescriptorImageInfo *ii = ii_; VkBufferView *bv = bv_;
    if (g_np) { if (D.n + 64 > 320) D.n = 320 - 64; w = D.w + D.n; bi = D.bi + D.n; ii = D.ii + D.n; bv = D.bv + D.n; }
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < s->nres; i++) {
        const spv_res *r = &s->res[i]; const vres_how *h = &s->how[i];
        w[nw] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = r->binding + s->shift, .descriptorCount = 1, .descriptorType = desc_type(r->kind) };
        if (r->count > 1) RED_ONCE(28, "shader %s: array binding %s (%u) gets its first element only", s->key, r->name, r->count);
        switch (r->kind) {
            case SPV_RES_UBO: case SPV_RES_SSBO: {
                VkBuffer b = g_zero_buf; VkDeviceSize o = 0, n = 65536;
                if (h->what == W_SUPPORT) b = g_support_buf;
                else if (h->what == W_C1) { b = s->c1buf; o = s->c1off; n = s->c1len; }
                else if (h->what == W_DRV0 || h->what == W_DRV2) {
                    int bank = h->what == W_DRV2 ? 2 : 0;
                    if (s->drv_read[bank]) {   // driver segments, filled from the current bindings (nvn_mtl.m encode_args_)
                        uint8_t *d = arena_alloc(g_sub, 0xA00, &b, &o); memset(d, 0, 0xA00); n = 0xA00;
                        if (!bank) {
                            int base = DM_GAME_NVN_SSBO_BASE(st);
                            if (base >= 0) for (int q = 0; q < 16; q++) { uint64_t a = S.ssbo[st][q].addr; uint32_t sz = (uint32_t)S.ssbo[st][q].size; memcpy(d + base + 16 * q, &a, 8); memcpy(d + base + 16 * q + 8, &sz, 4); }
                            if (st != 5) { const float mp = 2047.0f; memcpy(d + 0x610, &mp, 4); memcpy(d + 0x644, &g_draw_binst, 4); }
                            else { memcpy(d + 0x200, g_dispatch_dim, 12); g_c0_buf = b; g_c0_off = o + 0x200;
                                   for (int q = 0; q < 16; q++) { uint64_t a = S.ubo[5][q].addr; uint32_t sz = (uint32_t)S.ubo[5][q].size; memcpy(d + 0x210 + 16 * q, &a, 8); memcpy(d + 0x210 + 16 * q + 8, &sz, 4); } }
                        } else {
                            for (int q = 0; q < DM_GAME_NVN_TEX_SLOTS; q++) { uint32_t th = (uint32_t)S.tex[st][q]; memcpy(d + DM_GAME_NVN_TEX_BASE + 8 * q, &th, 4); }
                            for (int q = 0; q < DM_GAME_NVN_IMG_SLOTS; q++) { uint32_t ih = (uint32_t)S.img[st][q]; memcpy(d + DM_GAME_NVN_IMG_BASE + 8 * q, &ih, 4); }
                            for (int q = 0; q < 128; q++) { uint32_t sh = (uint32_t)S.samp[st][q]; memcpy(d + DM_GAME_NVN_SAMP_BASE + 8 * q, &sh, 4); }
                        }
                    }
                } else {
                    const p_bind_buf *u = NULL; uint64_t addr = 0, size = 0;
                    if (h->what == W_CBUF || h->what == W_UBOP) u = h->reg < 16 ? &S.ubo[st][h->reg] : NULL;
                    else if (h->what == W_SSBO) u = h->reg < 16 ? &S.ssbo[st][h->reg] : NULL;
                    if (u) { addr = u->addr; size = u->size; }
                    if (h->what == W_SBUFU) { uint32_t lo = ubo_word(st, h->reg, h->off), hi = ubo_word(st, h->reg, h->off + 4); addr = (uint64_t)hi << 32 | lo; size = 0; }
                    VkBuffer pb; VkDeviceSize po, left;
                    if (addr && pool_buf(addr, 1, &pb, &po, &left)) {
                        VkDeviceSize al = r->kind == SPV_RES_UBO ? g_props.limits.minUniformBufferOffsetAlignment : g_props.limits.minStorageBufferOffsetAlignment;
                        // the rest of the pool up to the block the shader declares (64 KB uniform blocks), not the game's bound size: Maxwell (and the Metal replay, which binds a raw
                        // address) reads past the bound size into the memory that follows, and Shield's lighting reads there (bound at the exact size, the scene came out at half brightness)
                        VkDeviceSize want = left, cap = r->kind == SPV_RES_UBO ? g_props.limits.maxUniformBufferRange : g_props.limits.maxStorageBufferRange; (void)size;
                        if (want > left) want = left; if (want > cap) want = cap;
                        if (po % al == 0) { b = pb; o = po; n = want; }
                        else if (r->kind == SPV_RES_UBO) {   // misaligned uniform data: a snapshot in the arena (the GPU-side updates of this submission are in the pool already)
                            if (want > 65536) want = 65536;   // the translated shaders declare 64 KB uniform blocks: never more to copy (MoltenVK reports a range of gigabytes)
                            uint8_t *d = arena_alloc(g_sub, want, &b, &o); memcpy(d, (const void *)(uintptr_t)addr, want); n = want;
                            RED_ONCE(29, "uniform buffer at pool offset %#llx is not %llu-aligned: bound as a snapshot", (unsigned long long)po, (unsigned long long)al);
                        } else if (!h->rw) {   // misaligned read-only storage data (Adreno wants 64, the Switch less): a snapshot as for uniforms, not zeros
                            VkDeviceSize m = size && size < want ? size : want; if (m > (size ? 1u << 20 : 65536u)) m = size ? 1u << 20 : 65536u;
                            uint8_t *d = arena_alloc(g_sub, m, &b, &o); memcpy(d, (const void *)(uintptr_t)addr, m); n = m;
                            RED_ONCE(30, "storage buffer at pool offset %#llx is not %llu-aligned: bound as a snapshot", (unsigned long long)po, (unsigned long long)al);
                        } else RED_ONCE(32, "writable storage buffer at pool offset %#llx is not %llu-aligned: bound to zeros", (unsigned long long)po, (unsigned long long)al);
                        if (r->kind == SPV_RES_SSBO && h->rw) wb_note(addr, size ? size : (want < 65536 ? want : 65536));   // a writable storage buffer: its bound range comes back (mirrored pools)
                    } else if (h->what != W_NONE && addr) RED_ONCE(31, "stage %d %s slot %d at %#llx is in no memory pool", st, r->kind == SPV_RES_UBO ? "uniform" : "storage", h->reg, (unsigned long long)addr);
                }
                if (r->kind == SPV_RES_UBO && n > g_props.limits.maxUniformBufferRange) n = g_props.limits.maxUniformBufferRange;
                bi[nw] = (VkDescriptorBufferInfo){ b, o, n ? n : 16 }; w[nw].pBufferInfo = &bi[nw]; nw++; break;
            }
            case SPV_RES_TEX: case SPV_RES_SIMG: case SPV_RES_IMG: {
                int storage = r->kind == SPV_RES_IMG; int tid = 0, sid = 0;
                if (h->what == W_TEX) { uint64_t th = h->reg < NTEX ? S.tex[st][h->reg] : 0; tid = (int)(th & 0xfffff); sid = (int)(th >> 20 & 0xfff); }
                else if (h->what == W_TEXU) { uint32_t th = ubo_word(st, h->reg, h->off); tid = (int)(th & 0xfffff); sid = (int)(th >> 20); }
                else if (h->what == W_IMG) tid = h->reg < 32 ? (int)(S.img[st][h->reg] & 0xfffff) : 0;
                if (h->skind == S_SEP) sid = h->sreg < 128 ? (int)(S.samp[st][h->sreg] >> 20) : 0;
                else if (h->skind == S_UBO) sid = (int)(ubo_word(st, h->sreg, h->soff) >> 20);
                else if (h->skind == S_ID) sid = h->sreg;
                VkImageView v = tid ? tex_view_for(tid, r, storage) : VK_NULL_HANDLE;
                if (!v) v = dummy_view(r, storage);
                VkSampler smp = storage ? VK_NULL_HANDLE : (tid && tex_is_int(tid) ? samp_nearest_by_id(sid) : samp_by_id(sid)); if (!storage && !smp) smp = g_dummy_samp;
                ii[nw] = (VkDescriptorImageInfo){ smp, v, VK_IMAGE_LAYOUT_GENERAL }; w[nw].pImageInfo = &ii[nw]; nw++; break;
            }
            case SPV_RES_SAMP: ii[nw] = (VkDescriptorImageInfo){ g_dummy_samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL }; w[nw].pImageInfo = &ii[nw]; nw++; RED_ONCE(32, "separate sampler binding %s not resolved", r->name); break;
            case SPV_RES_TBUF: case SPV_RES_IBUF: {
                int tid = h->reg < NTEX ? (int)(S.tex[st][h->reg] & 0xfffff) : 0; VkBufferView view = VK_NULL_HANDLE;
                if (r->kind == SPV_RES_IBUF) tid = h->reg < 32 ? (int)(S.img[st][h->reg] & 0xfffff) : 0;
                if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) { vtex *x = tex_res(&g_treg[tid].t); if (x && x->bview) view = x->bview; }
                bv[nw] = view ? view : g_dummy_tbuf[r->sclass < 3 ? r->sclass : 0]; w[nw].pTexelBufferView = &bv[nw]; nw++; break;
            }
        }
    }
    pthread_mutex_unlock(&g_rmu);
    if (g_np) D.n += nw;
    else if (nw) p_push(CB(), bp, pl, 0, nw, w);
}

// ================= Draws =================
static void draw(const p_draw *dw, const p_draw_idx *di) {
    g_draw_binst = (uint32_t)(dw ? dw->base_instance : di ? di->base_instance : 0);
    if ((dw && dw->mdi_cnt) || (di && di->mdi_cnt)) { uint64_t a = dw ? dw->mdi_cnt : di->mdi_cnt; int i = dw ? dw->mdi_i : di->mdi_i; uint32_t n = 0; memcpy(&n, (const void *)(uintptr_t)a, 4); if ((uint32_t)i >= n) return; }
    if (!S.prog) { RED_ONCE(33, "draw with no program bound"); return; }
    vprog *pr = prog_get(S.prog); if (!pr->sh[0]) { RED_ONCE(34, "program has no vertex shader (or it is not mapped)"); return; }
    // Rasterizer discard: the vertex stage alone. With transform feedback active the draw captures its outputs (the shader's capture variant stores them into the bound
    // feedback buffers, as nvn_mtl.m tf_draw: Pokémon Legends: Arceus / Z-A skin characters so); otherwise it changes nothing and is skipped
    VkBuffer tfb[4] = {0}; VkDeviceSize tfo[4] = {0}, tfr[4] = {0}; uint32_t tfpc[5] = {0}; int tfcap = 0;
    if (S.rast_discard) {
        vshader *vs = pr->sh[0];
        if (!S.tf_on) return;
        if (!vs->tfmod) { RED_ONCE(100, "transform feedback with vertex shader %s, which has no capture variant: nothing captured", vs->key); return; }
        if (!dw || dw->ind || dw->mdi_cnt || dw->instances > 1 || dw->prim == 6 || dw->prim == 7) { RED_ONCE(101, "transform feedback draw that is indexed, indirect, instanced or a fan / quad list (vertex %s): not captured", vs->key); return; }
        if (dw->count <= 0) return;
        VkDeviceSize al = g_props.limits.minStorageBufferOffsetAlignment ? g_props.limits.minStorageBufferOffsetAlignment : 256;
        tfpc[0] = (uint32_t)dw->first;
        for (int b = 0; b < 4; b++) { if (!vs->tf_stride[b]) continue;
            uint64_t n = (uint64_t)dw->count * vs->tf_stride[b], at = S.tf[b].addr + S.tf_off[b]; VkDeviceSize o;
            if (!S.tf[b].addr || S.tf_off[b] + n > S.tf[b].size || !pool_buf(at, n, &tfb[b], &o, NULL)) {
                RED_ONCE(102, "transform feedback buffer %d of vertex %s is unbound, too small or in no memory pool (%#llx + %llu of %llu): not captured", b, vs->key, (unsigned long long)S.tf[b].addr, (unsigned long long)(S.tf_off[b] + n), (unsigned long long)S.tf[b].size);
                return; }
            tfo[b] = o & ~(al - 1); tfr[b] = o - tfo[b] + n; tfpc[1 + b] = (uint32_t)((o - tfo[b]) / 4); }   // bound at an aligned offset; the shader starts base[b] words in
        tfcap = 1;
    }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK && !tfcap) return;
    if (tfcap) tf_pass_begin(); else if (!pass_begin()) return;
    int prim = dw ? dw->prim : di->prim; VkPrimitiveTopology topo = topo_of(prim);
    VkPipeline p = pso_get(pr, topo); if (!p) return;
    VkCommandBuffer cb = CB();
    { static VkCommandBuffer bcb; static VkPipeline bp; static uint64_t bfence;   // the same pipeline already bound in this command buffer: no rebind (Vulkan keeps it across passes)
      static int off = -1; if (off < 0) off = getenv("DM_VK_BIND_ALWAYS") != NULL;
      if (off || cb != bcb || bp != p || bfence != g_sub->fence) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bcb = cb; bp = p; bfence = g_sub->fence; } }
    // dynamic state
    float vx = 0, vy = 0, vw = (float)P.tw, vh = (float)P.th;
    if (S.has_vp) { vx = S.vp[0][0]; vy = S.vp[0][1]; vw = S.vp[0][2]; vh = S.vp[0][3]; }
    VkViewport v = { vx, vy, vw, vh, S.dr[0], S.dr[1] };
    if (!atomic_load(&g_origin_ll)) { v.y = vy + vh; v.height = -vh; }   // NVN upper-left: NDC +1 on the first row (Vulkan puts -1 there)
    if (v.minDepth < 0) v.minDepth = 0; if (v.minDepth > 1) v.minDepth = 1; if (v.maxDepth < 0) v.maxDepth = 0; if (v.maxDepth > 1) v.maxDepth = 1;
    vkCmdSetViewport(cb, 0, 1, &v);
    VkRect2D sc = {{0, 0}, {P.tw, P.th}};
    if (R.has_sc && !tfcap) { long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h; if (x1 > (long)P.tw) x1 = P.tw; if (y1 > (long)P.th) y1 = P.th;
        if (x1 <= x0 || y1 <= y0) return; sc = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}; }
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetCullMode(cb, S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    int ccw = S.poly.front == NVN_FRONT_FACE_CCW; if (atomic_load(&g_origin_ll)) ccw = !ccw;   // lower-left: the y flip relative to GL window coordinates reverses the winding
    vkCmdSetFrontFace(cb, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, topo);
    if (!tfcap) {   // a capture pipeline (rasterizer discard) has no depth / stencil state to set (VUID-vkCmdDraw-None-08608)
    vkCmdSetDepthTestEnable(cb, P.hd && S.ds.dt); vkCmdSetDepthWriteEnable(cb, P.hd && S.ds.dw); vkCmdSetDepthCompareOp(cb, (VkCompareOp)(S.ds.df ? S.ds.df - 1 : 7));
    vkCmdSetStencilTestEnable(cb, P.hs && S.ds.st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, (VkStencilOp)(S.ds.fail_f - 1), (VkStencilOp)(S.ds.zpass_f - 1), (VkStencilOp)(S.ds.zfail_f - 1), (VkCompareOp)(S.ds.sf_f - 1));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, (VkStencilOp)(S.ds.fail_b - 1), (VkStencilOp)(S.ds.zpass_b - 1), (VkStencilOp)(S.ds.zfail_b - 1), (VkCompareOp)(S.ds.sf_b - 1));
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_vmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_vmask[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_wmask[1]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, S.st_ref[1]);
    if (S.poly.offset_en & 4) vkCmdSetDepthBias(cb, S.po.units, S.po.clamp, S.po.factor); else vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetBlendConstants(cb, S.blend_color);
    }
    // vertex streams: the pool buffers at their addresses, strides from the stream state; binding 16 = zeros for unbound attributes
    VkBuffer vb[17]; VkDeviceSize vo[17], vs_[17];
    for (int s_ = 0; s_ < 16; s_++) { VkBuffer b; VkDeviceSize o;
        if (S.vbuf[s_].address && pool_buf(S.vbuf[s_].address, 1, &b, &o, NULL)) { vb[s_] = b; vo[s_] = o; } else { vb[s_] = g_zero_buf; vo[s_] = 0; }
        vs_[s_] = S.stream[s_].stride; }
    vb[16] = g_zero_buf; vo[16] = 0; vs_[16] = 0;
    vkCmdBindVertexBuffers2(cb, 0, 17, vb, vo, NULL, vs_);
    VkPipelineLayout pl = prog_layout(pr, 0); g_np = pr->np[0];
    for (int st = 0; st < 5; st++) push_stage(pr->sh[st], st, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    if (tfcap) {   // the capture buffers, then the draw, then its writes made visible to what reads them (vertex streams, shaders, copies)
        VkWriteDescriptorSet w_[4]; VkDescriptorBufferInfo bi_[4]; uint32_t nw = 0;
        VkWriteDescriptorSet *w = w_; VkDescriptorBufferInfo *bi = bi_;
        if (g_np) { if (D.n + 4 > 320) D.n = 316; w = D.w + D.n; bi = D.bi + D.n; }
        for (int b = 0; b < 4; b++) if (tfb[b]) { bi[nw] = (VkDescriptorBufferInfo){ tfb[b], tfo[b], tfr[b] };
            w[nw] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = TF_BIND + (uint32_t)b, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[nw] }; nw++; }
        if (g_np) D.n += nw; else if (nw) p_push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, nw, w);
        desc_flush(VK_PIPELINE_BIND_POINT_GRAPHICS, pl, pr->dsl[0]);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof tfpc, tfpc);
        vkCmdDraw(cb, (uint32_t)dw->count, 1, (uint32_t)dw->first, (uint32_t)dw->base_instance);
        vshader *vs = pr->sh[0];
        for (int b = 0; b < 4; b++) if (tfb[b]) { uint64_t n = (uint64_t)dw->count * vs->tf_stride[b]; wb_note(S.tf[b].addr + S.tf_off[b], n); S.tf_off[b] += n; }
        pass_end();
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        { static int once; if (!once++) VLOG("transform feedback: first capture (vertex %s, %d vertices)", vs->key, dw->count); }
        return;
    }
    desc_flush(VK_PIPELINE_BIND_POINT_GRAPHICS, pl, pr->dsl[0]);
    uint32_t inst = (uint32_t)((dw ? dw->instances : di->instances) > 0 ? (dw ? dw->instances : di->instances) : 1);
    if ((dw && dw->ind) || (di && di->ind)) {
        VkBuffer ab; VkDeviceSize ao; uint64_t ind = dw ? dw->ind : di->ind;
        if (prim == 6 || prim == 7) { RED_ONCE(35, "indirect fan / quad draw not supported yet"); return; }
        if (!pool_buf(ind, dw ? 16 : 20, &ab, &ao, NULL)) { RED_ONCE(36, "indirect arguments in no pool"); return; }
        if (dw) vkCmdDrawIndirect(cb, ab, ao, 1, 16);
        else { VkBuffer ib; VkDeviceSize io; if (di->itype == 0) { RED_ONCE(37, "indirect draw with 8-bit indices not supported yet"); return; }
               if (!pool_buf(di->ibuf, 1, &ib, &io, NULL)) { RED_ONCE(38, "index buffer in no pool"); return; }
               vkCmdBindIndexBuffer(cb, ib, io, di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32); vkCmdDrawIndexedIndirect(cb, ab, ao, 1, 20); }
        return;
    }
    if (dw && (prim == 6 || prim == 7)) {   // fans / quads: expanded into a 32-bit triangle index list in the arena
        uint32_t nt = prim == 7 ? (uint32_t)(dw->count / 4) * 2 : dw->count >= 3 ? (uint32_t)dw->count - 2 : 0; if (!nt) return;
        VkBuffer ib; VkDeviceSize io; uint32_t *d = (uint32_t *)arena_alloc(g_sub, (VkDeviceSize)nt * 12, &ib, &io);
        for (uint32_t t = 0; t < nt; t++) { if (prim == 6) { d[3 * t] = 0; d[3 * t + 1] = t + 1; d[3 * t + 2] = t + 2; } else { uint32_t q = (t >> 1) * 4, hh = t & 1; d[3 * t] = q; d[3 * t + 1] = q + 1 + hh; d[3 * t + 2] = q + 2 + hh; } }
        vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, nt * 3, inst, 0, dw->first, (uint32_t)dw->base_instance); return;
    }
    if (dw) { if (dw->count) vkCmdDraw(cb, (uint32_t)dw->count, inst, (uint32_t)dw->first, (uint32_t)dw->base_instance); return; }
    if (!di->count) return;
    int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
    if (di->itype == 0 || prim == 6 || prim == 7) {   // 8-bit indices / indexed fans and quads: rewritten into 16- / 32-bit indices in the arena from guest memory
        const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf; if (!src) return;
        #define IX(i) (isz == 1 ? (uint32_t)src[i] : isz == 2 ? (uint32_t)((const uint16_t *)src)[i] : ((const uint32_t *)src)[i])
        VkBuffer ib; VkDeviceSize io; uint32_t n;
        if (prim == 6 || prim == 7) {
            uint32_t nt = prim == 7 ? (uint32_t)(di->count / 4) * 2 : di->count >= 3 ? (uint32_t)di->count - 2 : 0; if (!nt) return;
            uint32_t *d = (uint32_t *)arena_alloc(g_sub, (VkDeviceSize)nt * 12, &ib, &io);
            for (uint32_t t = 0; t < nt; t++) { if (prim == 6) { d[3 * t] = IX(0); d[3 * t + 1] = IX(t + 1); d[3 * t + 2] = IX(t + 2); } else { uint32_t q = (t >> 1) * 4, hh = t & 1; d[3 * t] = IX(q); d[3 * t + 1] = IX(q + 1 + hh); d[3 * t + 2] = IX(q + 2 + hh); } }
            vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT32); n = nt * 3;
        } else {
            uint16_t *d = (uint16_t *)arena_alloc(g_sub, (VkDeviceSize)di->count * 2, &ib, &io); for (int32_t i = 0; i < di->count; i++) d[i] = src[i];
            vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT16); n = (uint32_t)di->count;
        }
        #undef IX
        vkCmdDrawIndexed(cb, n, inst, 0, di->base_vertex, (uint32_t)di->base_instance); return;
    }
    VkBuffer ib; VkDeviceSize io;
    if (!pool_buf(di->ibuf, (uint64_t)di->count * isz, &ib, &io, NULL)) { RED_ONCE(38, "index buffer in no pool"); return; }
    vkCmdBindIndexBuffer(cb, ib, io, di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, (uint32_t)di->count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
}
static nvn_hmap g_cps = {.klen = sizeof(void *)};
static void dispatch(const p_dispatch *dp) {
    if (!S.cprog) { RED_ONCE(39, "dispatch with no program bound"); return; }
    vprog *pr = prog_get(S.cprog); vshader *cs = pr->sh[5]; if (!cs) { RED_ONCE(40, "dispatched program has no compute shader (or it is not mapped)"); return; }
    VkPipelineLayout pl = prog_layout(pr, 1); g_np = pr->np[1];
    void *key = pr; VkPipeline p = nvn_hm_get(&g_cps, &key);
    if (!p) { VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs->mod, .pName = "main" }, .layout = pl };
        if (vkCreateComputePipelines(g_dev, g_pcache, 1, &ci, NULL, &p)) { VLOG("[RED] compute pipeline %s failed", cs->key); return; }
        nvn_hm_put(&g_cps, &key, p); }
    outside_pass();
    VkCommandBuffer cb = CB();
    g_dispatch_dim[0] = (uint32_t)dp->x; g_dispatch_dim[1] = (uint32_t)dp->y; g_dispatch_dim[2] = (uint32_t)dp->z; g_c0_buf = VK_NULL_HANDLE;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    push_stage(cs, 5, VK_PIPELINE_BIND_POINT_COMPUTE, pl);
    desc_flush(VK_PIPELINE_BIND_POINT_COMPUTE, pl, pr->dsl[1]);
    if (dp->ind) { VkBuffer ab; VkDeviceSize ao; if (!pool_buf(dp->ind, 12, &ab, &ao, NULL)) { RED_ONCE(41, "indirect dispatch arguments in no pool"); return; }
        if (g_c0_buf) { VkBufferCopy c = { ao, g_c0_off, 12 }; vkCmdCopyBuffer(cb, ab, g_c0_buf, 1, &c); barrier_all(cb); }   // the shader reads the counts from c0 + 0x200
        vkCmdDispatchIndirect(cb, ab, ao); }
    else vkCmdDispatch(cb, (uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z);
    barrier_all(cb);
}

// ================= Buffer operations (in submission order) =================
static void sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    VkBuffer sb, db; VkDeviceSize so, dof;
    if (!pool_buf(src, n, &sb, &so, NULL) || !pool_buf(dst, n, &db, &dof, NULL)) { RED_ONCE(42, "buffer copy %#llx → %#llx outside the pools", (unsigned long long)src, (unsigned long long)dst); return; }
    outside_pass();
    if (sb == db && so < dof + n && dof < so + n) {   // overlapping ranges of one buffer: through the arena
        VkBuffer tb; VkDeviceSize to; arena_alloc(g_sub, n, &tb, &to);
        VkBufferCopy c1 = { so, to, n }; vkCmdCopyBuffer(CB(), sb, tb, 1, &c1); barrier_all(CB()); VkBufferCopy c2 = { to, dof, n }; vkCmdCopyBuffer(CB(), tb, db, 1, &c2);
    } else { VkBufferCopy c = { so, dof, n }; vkCmdCopyBuffer(CB(), sb, db, 1, &c); }
    barrier_all(CB()); wb_note(dst, n);
}
static void sub_update(const void *data, uint64_t dst, uint64_t n) {   // the data is copied now (the game may reuse its command memory before the GPU runs)
    if (!n) return;
    VkBuffer db; VkDeviceSize dof; if (!pool_buf(dst, n, &db, &dof, NULL)) { RED_ONCE(43, "uniform update outside the pools"); return; }
    outside_pass();
    VkBuffer tb; VkDeviceSize to; void *m = arena_alloc(g_sub, n, &tb, &to); memcpy(m, data, n);
    VkBufferCopy c = { to, dof, n }; vkCmdCopyBuffer(CB(), tb, db, 1, &c); barrier_all(CB()); wb_note(dst, n);
}
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!n) return;
    VkBuffer db; VkDeviceSize dof; if (!pool_buf(dst, n, &db, &dof, NULL)) { RED_ONCE(44, "buffer fill outside the pools"); return; }
    outside_pass();
    if (dof % 4 || n % 4) { RED_ONCE(45, "buffer fill not 4-byte aligned (%#llx, %llu)", (unsigned long long)dof, (unsigned long long)n); return; }
    vkCmdFillBuffer(CB(), db, dof, n, value); barrier_all(CB()); wb_note(dst, n);
}

// ================= Texture copies and clears =================
static void copy_b2t(const p_copy_b2t *c, int to_buffer) {
    vtex *x = tex_res(c->tex); if (!x || !x->img) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh, img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;
    int nz = c->r.depth ? c->r.depth : 1;
    VkBuffer b; VkDeviceSize o; if (!pool_buf(c->src, img * nz, &b, &o, NULL)) { RED_ONCE(46, "texture copy buffer side outside the pools"); return; }
    if (row % bpb || (img % row)) { RED_ONCE(47, "texture copy pitch %zu not a whole number of %d-byte blocks", row, bpb); return; }
    int bck = nvn_fmt_vk_decode(c->tex->d.format);
    if (bck && to_buffer) { RED_ONCE(98, "a BC texture decoded on upload copied back to a buffer: skipped (format %#x)", c->tex->d.format); return; }
    if (bck) {   // BC blocks the game copies in: decoded now (the CPU has written them) into the submission's arena, then copied as plain texels
        const uint8_t *src = (const uint8_t *)(uintptr_t)c->src; int tb = bc_texel_bytes(bck); size_t orow = (size_t)c->r.width * tb, oimg = orow * (size_t)c->r.height;
        VkBuffer sb; VkDeviceSize so; uint8_t *sp = arena_alloc(g_sub, oimg * nz ? oimg * nz : 16, &sb, &so);
        for (int s = 0; s < nz; s++) bc_decode_image(bck, src + s * img, row, c->r.width, c->r.height, sp + s * oimg, orow);
        b = sb; o = so; row = orow; img = oimg; bpb = tb; bw = bh = 1;
    }
    outside_pass();
    VkBufferImageCopy r = { .bufferOffset = o, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(img / row * bh),
        .imageSubresource = { x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : x->aspect, level, x->is3d ? 0 : layer0 + (uint32_t)c->r.zoffset, x->is3d ? 1 : (uint32_t)nz },
        .imageOffset = { c->r.xoffset, c->r.yoffset, x->is3d ? c->r.zoffset : 0 }, .imageExtent = { (uint32_t)c->r.width, (uint32_t)c->r.height, x->is3d ? (uint32_t)nz : 1 } };
    if (to_buffer) vkCmdCopyImageToBuffer(CB(), x->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r); else vkCmdCopyBufferToImage(CB(), b, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    barrier_all(CB()); if (to_buffer) wb_note(c->src, img * nz);
}
static void copy_t2t(const p_copy_t2t *c) {
    pass_flush();
    vtex *s = tex_res(c->src), *d = tex_res(c->dst); if (!s || !d || !s->img || !d->img) return;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    int nz = c->sr.depth ? c->sr.depth : 1;
    outside_pass();
    VkImageSubresourceLayers ss = { s->aspect, sl, s->is3d ? 0 : sz0 + (uint32_t)c->sr.zoffset, s->is3d ? 1 : (uint32_t)nz }, ds = { d->aspect, dl, d->is3d ? 0 : dz0 + (uint32_t)c->dr.zoffset, d->is3d ? 1 : (uint32_t)nz };
    if (c->flags & DM_COPY_RESOLVE) {
        if (s->samples > 1 && d->samples == 1 && s->fmt == d->fmt) { VkImageResolve r = { ss, {0, 0, 0}, ds, {0, 0, 0}, { s->w, s->h, 1 } }; vkCmdResolveImage(CB(), s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); }
        else if (s->samples == d->samples && s->fmt == d->fmt) { VkImageCopy r = { ss, {0, 0, 0}, ds, {0, 0, 0}, { s->w, s->h, 1 } }; vkCmdCopyImage(CB(), s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); }
        else RED_ONCE(48, "downsample case not implemented");
        barrier_all(CB()); return;
    }
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && s->fmt == d->fmt && !(c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT)) && s->samples == d->samples;
    if (same) { VkImageCopy r = { ss, { c->sr.xoffset, c->sr.yoffset, s->is3d ? c->sr.zoffset : 0 }, ds, { c->dr.xoffset, c->dr.yoffset, d->is3d ? c->dr.zoffset : 0 }, { (uint32_t)c->sr.width, (uint32_t)c->sr.height, s->is3d ? (uint32_t)nz : 1 } };
        vkCmdCopyImage(CB(), s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); }
    else {   // format change / scaling / mirroring: a blit (filtered when asked; depth and integer formats nearest)
        int32_t sx0 = c->sr.xoffset, sy0 = c->sr.yoffset, sx1 = sx0 + c->sr.width, sy1 = sy0 + c->sr.height;
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int32_t t = sx0; sx0 = sx1; sx1 = t; }
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int32_t t = sy0; sy0 = sy1; sy1 = t; }
        VkImageBlit r = { ss, {{sx0, sy0, 0}, {sx1, sy1, 1}}, ds, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}} };
        int lin = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && !(s->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT);
        if (s->samples > 1 || d->samples > 1) RED_ONCE(49, "multisample texture blit not supported");
        else vkCmdBlitImage(CB(), s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    barrier_all(CB());
}
static void clear_tex(const p_clear_tex *c) {
    vtex *x = tex_res(c->tex); if (!x || !x->img) return;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    int nz = c->r.depth ? c->r.depth : 1;
    uint32_t w = x->w >> lv ? x->w >> lv : 1, h = x->h >> lv ? x->h >> lv : 1;
    if (c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width != w || (uint32_t)c->r.height != h || (c->mask & 0xf) != 0xf) RED_ONCE(50, "partial texture clear (region / mask): cleared whole");
    outside_pass();
    VkImageSubresourceRange rg = { x->aspect, lv, 1, x->is3d ? 0 : l0 + (uint32_t)c->r.zoffset, x->is3d ? 1 : (uint32_t)nz };
    if (x->aspect & VK_IMAGE_ASPECT_COLOR_BIT) { VkClearColorValue v; memcpy(&v, c->c.u, 16); vkCmdClearColorImage(CB(), x->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg); }
    else { VkClearDepthStencilValue v = { c->c.f[0], x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? c->c.u[1] : c->c.u[0] }; vkCmdClearDepthStencilImage(CB(), x->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg); }
    barrier_all(CB());
}
static int scissor_full(const tex_t *t, const view_t *v, int has_v) {
    unsigned lv = has_v && v->set_levels ? v->base_level : 0; uint32_t w = t->d.width >> lv ? t->d.width >> lv : 1, h = (t->d.target == 0 || t->d.target == 3 ? 1 : t->d.height) >> lv; if (!h) h = 1;
    return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h);
}
static void clear_in_pass(int color_index, const VkClearValue *v, VkImageAspectFlags asp) {   // a clear inside an open pass (or a partial one): vkCmdClearAttachments over the scissor
    if (!pass_begin()) return;
    VkClearAttachment a = { asp, (uint32_t)(color_index < 0 ? 0 : color_index), *v };
    VkRect2D rc = {{0, 0}, {P.tw, P.th}};
    if (R.has_sc) { long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h; if (x1 > (long)P.tw) x1 = P.tw; if (y1 > (long)P.th) y1 = P.th;
        if (x1 <= x0 || y1 <= y0) return; rc = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}; }
    VkClearRect cr = { rc, 0, 1 };
    vkCmdClearAttachments(CB(), 1, &a, 1, &cr);
}

// ================= Command replay =================
static void bh_fresh(const p_bind_handles *b) { if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n); }
static int state_op(uint32_t op, const void *a) {
    switch (op) {
        case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
        case OP_BIND_PROGRAM: { const p_bind_program *b = a; if (b->stages & 0x20) S.cprog = b->prog; if (b->stages & 0x1f) S.prog = bind_stages(b->prog, b->stages & 0x1f); break; }
        case OP_BIND_STATE: { const p_bind_state *b = a; blend_st bs;
            switch (b->kind) { case 0: memcpy(&bs, b->bytes, sizeof bs); S.blend[bs.target] = bs; break; case 1: memcpy(&S.color, b->bytes, sizeof S.color); break; case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break; case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break; case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break; }
            break; }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first + b->n <= NTEX) memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first + b->n <= 128) memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); break; }
        case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; } break; }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_COPY_STRIDE: { const p_copy_stride *cs = a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
        case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; break;
        case OP_TF_BIND: { const p_tf_bind *b = a; if (b->index >= 0 && b->index < 4) S.tf[b->index] = *b; break; }
        case OP_TF_STATE: { const p_tf_state *b = a;   // Begin writes from the start of each buffer; Pause / Resume keep the offsets (as nvn_mtl.m: the host keeps them, the game's control buffer is not used)
            if (b->what == 0) { S.tf_on = 1; memset(S.tf_off, 0, sizeof S.tf_off); } else S.tf_on = b->what == 3; break; }
        default: return 0;
    }
    return 1;
}
static uint64_t g_stat_draws, g_stat_passes, g_stat_rt_same;
// The same render targets set again while their pass is open: the pass goes on (on a tiler every pass end stores the attachments and every begin loads them back;
// Legends: Arceus sets render targets about 120 times a frame). Views compared field by field where set
static int rt_same(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n || a->depth != b->depth || a->has_dview != b->has_dview || (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview))) return 0;
    for (int i = 0; i < a->n && i < 8; i++) if (a->color[i] != b->color[i] || a->has_cview[i] != b->has_cview[i] || (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i]))) return 0;
    return 1;
}
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec(const uint8_t *p) {
    if (!g_sub) { RED_ONCE(52, "commands replayed outside a submission"); return; }
    for (int guard = 0; guard < (1 << 26); guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_BEGIN: break;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); post((post_op){1, val, m, v, 0, 0}); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); post((post_op){2, NULL, 0, 0, ad, (uint32_t)t}); break; }
            case OP_RESET_COUNTER: case OP_RENDER_COND: break;
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); sub_update(a + 16, d, n); break; }
            case OP_SET_RT: g_stat_passes++; if (P.open && R.has_rt && !getenv_nomerge() && rt_same(&R.rt, (const p_set_rt *)a)) { g_stat_rt_same++; break; }
                pass_flush(); R.rt = *(const p_set_rt *)a; R.has_rt = 1; break;
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = (const p_clear_color *)a;
                if (c->index < 0 || c->index >= R.rt.n || !R.rt.color[c->index]) { RED_ONCE(53, "colour clear target %d invalid", c->index); break; }
                VkClearColorValue cv; memcpy(cv.float32, c->rgba, 16);
                { vtex *cx = tex_res(R.rt.color[c->index]); VkFormat cf = cx ? cx->fmt : VK_FORMAT_UNDEFINED;
                  if (R.rt.has_cview[c->index] && R.rt.cview[c->index].set_format) { uint32_t vf = 0; uint8_t sw[4]; if (nvn_fmt_to_vk(R.rt.cview[c->index].format, &vf, sw) && vf) cf = (VkFormat)vf; }
                  cv = clear_for(cf, c->rgba); }
                if (P.open || c->mask != 0xf || !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index])) {
                    if (c->mask != 0xf) RED_ONCE(54, "colour clear with a partial channel mask: all channels cleared");
                    VkClearValue v = { .color = cv }; clear_in_pass(c->index, &v, VK_IMAGE_ASPECT_COLOR_BIT); break; }
                R.clear_c[c->index] = 1; R.cc[c->index] = cv; break;
            }
            case OP_CLEAR_DS: {
                const p_clear_ds *c = (const p_clear_ds *)a;
                if (P.open || (R.rt.depth && !scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview))) {
                    VkClearValue v = { .depthStencil = { c->depth, (uint32_t)c->stencil } };
                    VkImageAspectFlags asp = (c->depth_mask && P.hd ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (c->stencil_mask && P.hs ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                    if (!P.open) pass_begin();
                    asp = (c->depth_mask && P.hd ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (c->stencil_mask && P.hs ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                    if (asp) clear_in_pass(-1, &v, asp); break; }
                if (c->depth_mask) { R.clear_d = 1; R.cd = c->depth; }
                if (c->stencil_mask) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
                break;
            }
            case OP_DRAW: draw((const p_draw *)a, NULL); g_stat_draws++; break;
            case OP_DRAW_IDX: draw(NULL, (const p_draw_idx *)a); g_stat_draws++; break;
            case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a, 0); break;
            case OP_COPY_T2B: copy_b2t((const p_copy_b2t *)a, 1); break;
            case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
            case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
            case OP_DISPATCH: dispatch((const p_dispatch *)a); break;
            case OP_BARRIER: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            default: state_op(h.op, a); break;
        }
        p += h.size;
    }
    RED_ONCE(55, "command list did not end");
}

// ================= CPU texture reads =================
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb) return;
    if (nvn_fmt_vk_decode(t->d.format)) { RED_ONCE(99, "CPU read of a BC texture decoded on upload (format %#x): not available", t->d.format); return; }
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh; int nz = r->depth ? r->depth : 1;
    vtex *x = tex_res(t); if (!x || !x->img) return;
    VkDeviceSize n = wb * hb * nz; vmalloc_t a; void *m; VkBuffer b = buf_make(n ? n : 16, &a, &m);
    VkCommandBuffer cb = once_begin();
    VkBufferImageCopy rg = { .bufferOffset = 0, .imageSubresource = { x->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : x->aspect, level, x->is3d ? 0 : layer0 + (uint32_t)r->zoffset, x->is3d ? 1 : (uint32_t)nz },
        .imageOffset = { r->xoffset, r->yoffset, x->is3d ? r->zoffset : 0 }, .imageExtent = { (uint32_t)r->width, (uint32_t)r->height, x->is3d ? (uint32_t)nz : 1 } };
    vkCmdCopyImageToBuffer(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &rg);
    once_end(cb);
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, (uint8_t *)m + (z * hb + y) * wb, wb);
    vkDestroyBuffer(g_dev, b, NULL); vk_free(a);
}

// ================= Present: no window yet (the Android surface comes with the app); paced to the game's period, screenshots on request =================
static _Atomic uint64_t g_present_n; static uint64_t g_last_present;
static uint32_t crc_tab[256];
static uint32_t crc32_(uint32_t c, const uint8_t *p, size_t n) { if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t r = i; for (int k = 0; k < 8; k++) r = r & 1 ? 0xedb88320u ^ (r >> 1) : r >> 1; crc_tab[i] = r; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8); return ~c; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3] };
    fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f); uint32_t c = crc32_(crc32_(0, h + 4, 4), d, n); uint8_t cc[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c }; fwrite(cc, 1, 4, f);
}
static void png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, int flip) {   // uncompressed (stored deflate blocks): no zlib needed
    size_t raw = (size_t)h * (1 + 4 * w); uint8_t *r = malloc(raw);
    for (uint32_t y = 0; y < h; y++) { r[y * (1 + 4 * w)] = 0; memcpy(r + y * (1 + 4 * w) + 1, rgba + (size_t)(flip ? h - 1 - y : y) * 4 * w, 4 * w); }
    size_t nb = (raw + 65534) / 65535, zn = 2 + raw + 5 * nb + 4; uint8_t *z = malloc(zn), *q = z; *q++ = 0x78; *q++ = 0x01;
    uint32_t a1 = 1, a2 = 0; for (size_t i = 0; i < raw; i++) { a1 = (a1 + r[i]) % 65521; a2 = (a2 + a1) % 65521; }
    for (size_t o = 0; o < raw; o += 65535) { size_t l = raw - o < 65535 ? raw - o : 65535; *q++ = o + l == raw; *q++ = (uint8_t)l; *q++ = (uint8_t)(l >> 8); *q++ = (uint8_t)~l; *q++ = (uint8_t)(~l >> 8); memcpy(q, r + o, l); q += l; }
    uint32_t ad = a2 << 16 | a1; *q++ = (uint8_t)(ad >> 24); *q++ = (uint8_t)(ad >> 16); *q++ = (uint8_t)(ad >> 8); *q++ = (uint8_t)ad;
    FILE *f = fopen(path, "wb"); if (!f) { VLOG("[RED] cannot write %s", path); free(r); free(z); return; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 }; fwrite(sig, 1, 8, f);
    uint8_t ih[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0 };
    png_chunk(f, "IHDR", ih, 13); png_chunk(f, "IDAT", z, (uint32_t)(q - z)); png_chunk(f, "IEND", NULL, 0); fclose(f); free(r); free(z);
}
static void shot_ex(vtex *x, const int crop[4], const char *path, uint32_t lv, uint32_t sl);
static void shot(vtex *x, const int crop[4], const char *path) { shot_ex(x, crop, path, 0, 0); }
static void shot_level(vtex *x, uint32_t lv, uint32_t sl, const char *path) { shot_ex(x, NULL, path, lv, sl); }
static void shot_ex(vtex *x, const int crop[4], const char *path, uint32_t lv, uint32_t sl) {
    uint32_t lw = x->w >> lv ? x->w >> lv : 1, lh = x->h >> lv ? x->h >> lv : 1;
    uint32_t cx = crop ? (uint32_t)crop[0] : 0, cy = crop ? (uint32_t)crop[1] : 0, w = crop && crop[2] ? (uint32_t)crop[2] : lw, h = crop && crop[3] ? (uint32_t)crop[3] : lh;
    mtl_finish();
    if (!(x->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || x->samples > 1 || fmt_int_class(x->fmt)) { VLOG("[texdump] %s: depth / multisample image not saved", path); return; }
    // a blit into an RGBA8 image converts any colour format, then a copy into host memory
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkImage im; VKCHECK(vkCreateImage(g_dev, &ci, NULL, &im)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, im, &rq); vmalloc_t ia = vk_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); vkBindImageMemory(g_dev, im, ia.b->mem, ia.off);
    vmalloc_t ba; void *m; VkBuffer b = buf_make((VkDeviceSize)w * h * 4, &ba, &m);
    VkCommandBuffer cb = once_begin();
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, lv, sl, 1 }, {{(int32_t)cx, (int32_t)cy, 0}, {(int32_t)(cx + w), (int32_t)(cy + h), 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}} };
    // 8-bit RGBA / BGRA (sRGB or not): the stored bytes as they are, which is what a display shows (a blit would decode sRGB into linear values and darken the picture);
    // other formats are converted by a blit
    int raw8 = x->fmt == VK_FORMAT_R8G8B8A8_UNORM || x->fmt == VK_FORMAT_R8G8B8A8_SRGB, bgra8 = x->fmt == VK_FORMAT_B8G8R8A8_UNORM || x->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (raw8 || bgra8) { VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT, lv, sl, 1 }, {(int32_t)cx, (int32_t)cy, 0}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {0, 0, 0}, { w, h, 1 } };
                         vkCmdCopyImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_GENERAL, 1, &ic); }
    else vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    barrier_all(cb);
    VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } }; vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_GENERAL, b, 1, &rg);
    once_end(cb);
    for (uint32_t i = 0; i < w * h; i++) { uint8_t *q = (uint8_t *)m + 4 * i; if (bgra8) { uint8_t t = q[0]; q[0] = q[2]; q[2] = t; } q[3] = 255; }
    png_write(path, m, w, h, atomic_load(&g_origin_ll));
    vkDestroyImage(g_dev, im, NULL); vk_free(ia); vkDestroyBuffer(g_dev, b, NULL); vk_free(ba);
    VLOG("screenshot %s (%ux%u)", path, w, h);
}
// ---------- Window presentation (Android: the app's ANativeWindow, front_metal_layer()) ----------
// A FIFO swapchain the size of the window; each present blits the game's window texture into it, scaled to fit with black bars, flipped in GL (lower-left) mode,
// through a swapchain format of the same sRGB-ness as the game's texture so its bytes reach the screen unchanged
void *front_metal_layer(void);
static struct { void *win; VkSurfaceKHR surf; VkSwapchainKHR sc; VkImage img[8]; VkImageView view[8]; uint32_t n; VkFormat fmt; VkExtent2D ext; int srgb;
                VkSemaphore acq[3], done[3]; VkFence fence[3]; VkCommandPool pool; VkCommandBuffer cb[3]; uint32_t slot; } W;
// Making and dropping the window surface runs on a thread of its own: present_window runs on a game thread, whose stack is the game's (outside its pthread stack), and
// releasing an Android surface calls into Java (BLASTBufferQueue's corner-radii callback), which attaches the calling thread; ART aborts the process when that thread's
// stack is not its pthread stack (Thread::InitStack: "FindStackTop() > GetStackEnd()"). That was the crash on leaving the app (recents, home) and coming back
static void swap_destroy_now(void);
static int swap_ensure_now(void *win, int srgb);
struct swap_job { int make; void *win; int srgb, r; };
static void *swap_job_main(void *p) {
    struct swap_job *j = p;
    if (j->make) j->r = swap_ensure_now(j->win, j->srgb); else swap_destroy_now();
    return NULL;
}
static int swap_job_run(int make, void *win, int srgb) {
    struct swap_job j = { make, win, srgb, 0 }; pthread_t t;
    if (pthread_create(&t, NULL, swap_job_main, &j)) { RED_ONCE(59, "window: no thread for the swapchain"); return 0; }
    pthread_join(t, NULL);
    return j.r;
}
static void swap_destroy(void) { if (W.sc || W.surf) swap_job_run(0, NULL, 0); W.win = NULL; }
static int swap_ensure(void *win, int srgb) {
    if (W.sc && W.win == win && W.srgb == srgb) return 1;
    return swap_job_run(1, win, srgb);
}
static void swap_destroy_now(void) {
    if (W.sc) { vkDeviceWaitIdle(g_dev); for (uint32_t i = 0; i < W.n; i++) if (W.view[i]) { vkDestroyImageView(g_dev, W.view[i], NULL); W.view[i] = VK_NULL_HANDLE; }
                vkDestroySwapchainKHR(g_dev, W.sc, NULL); W.sc = VK_NULL_HANDLE; }
    if (W.surf) { vkDestroySurfaceKHR(g_inst, W.surf, NULL); W.surf = VK_NULL_HANDLE; }
    W.win = NULL;
}
static int swap_ensure_now(void *win, int srgb) {
    swap_destroy_now();
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR si = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = (ANativeWindow *)win };
    if (vkCreateAndroidSurfaceKHR(g_inst, &si, NULL, &W.surf)) { RED_ONCE(56, "cannot create the window surface"); return 0; }
#else
    (void)win; return 0;
#endif
    VkBool32 ok = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, W.surf, &ok); if (!ok) { RED_ONCE(57, "the queue cannot present to the window"); return 0; }
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, W.surf, &caps);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, NULL); VkSurfaceFormatKHR fs[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, fs);
    VkFormat want = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; W.fmt = fs[0].format;
    for (uint32_t i = 0; i < nf; i++) if (fs[i].format == want) W.fmt = want;
    W.ext = caps.currentExtent;
#ifdef __ANDROID__
    if (W.ext.width == 0xffffffffu || 1) { W.ext.width = (uint32_t)ANativeWindow_getWidth((ANativeWindow *)win); W.ext.height = (uint32_t)ANativeWindow_getHeight((ANativeWindow *)win); }
#endif
    // MAILBOX when the window has it: the game paces its own frames (mtl_present), and with FIFO a slow compositor (a screen recording, the notification shade) held
    // presentation, which shares the queue lock with the game's submissions, so the whole game waited on the compositor (LAS Recomp's Android port measured 4–21 fps with
    // the GPU mostly idle under FIFO, a steady rate with MAILBOX). DM_VK_PRESENT=fifo|mailbox|immediate to choose
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    { const char *e = getenv("DM_VK_PRESENT"); VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
      uint32_t npm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, NULL); VkPresentModeKHR pms[16]; if (npm > 16) npm = 16; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, pms);
      for (uint32_t i = 0; i < npm; i++) if (pms[i] == want) pm = want; }
    uint32_t n = caps.minImageCount + (pm == VK_PRESENT_MODE_MAILBOX_KHR ? 2 : 1); if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = W.surf, .minImageCount = n, .imageFormat = W.fmt, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = W.ext, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .presentMode = pm, .clipped = VK_TRUE };
    VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &W.sc);
    if (r) { RED_ONCE(58, "swapchain creation failed: %d", r); return 0; }
    W.n = 8; vkGetSwapchainImagesKHR(g_dev, W.sc, &W.n, W.img);
    for (uint32_t i = 0; i < W.n; i++) { VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = W.img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = W.fmt,
                                           .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }; vkCreateImageView(g_dev, &vci, NULL, &W.view[i]); }
    if (!W.pool) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf }; vkCreateCommandPool(g_dev, &pci, NULL, &W.pool);
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = W.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3 }; vkAllocateCommandBuffers(g_dev, &cai, W.cb);
        for (int i = 0; i < 3; i++) { VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; vkCreateSemaphore(g_dev, &sci, NULL, &W.acq[i]); vkCreateSemaphore(g_dev, &sci, NULL, &W.done[i]);
            VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT }; vkCreateFence(g_dev, &fci, NULL, &W.fence[i]); }
    }
    W.win = win; W.srgb = srgb;
    VLOG("window: swapchain %ux%u, %u images, format %d, %s", W.ext.width, W.ext.height, W.n, W.fmt, pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : pm == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : "immediate");
    return 1;
}
// ---------- Touch overlay (nvn_ovl.h): drawn into the swapchain image after the game picture, one quad per control ----------
#include "overlay/ovl_spv.h"
#include "overlay/ovl_font.h"
__attribute__((weak)) int front_overlay(dm_ovl_item *it, int max, int width, int height) { (void)it; (void)max; (void)width; (void)height; return 0; }
static struct { VkFormat fmt; VkPipeline pipe; VkPipelineLayout pl; VkDescriptorSetLayout dsl; VkDescriptorPool dp; VkDescriptorSet ds; VkSampler samp;
                VkImage font; VkImageView fview; vmalloc_t fa, sa; VkBuffer stage; int font_ready, failed; } O;
typedef struct { float rect[4], color[4], shape[4], glyph[4], target[2]; } ovl_pc;
static int ovl_setup(VkFormat fmt) {
    if (O.failed) return 0;
    if (O.pipe && O.fmt == fmt) return 1;
    if (O.pipe) { vkDestroyPipeline(g_dev, O.pipe, NULL); O.pipe = VK_NULL_HANDLE; }
    if (!O.pl) {
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f };
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8_UNORM, .extent = { ovl_font_w, ovl_font_h, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
        if (vkCreateSampler(g_dev, &sci, NULL, &O.samp) || vkCreateImage(g_dev, &ci, NULL, &O.font)) { O.failed = 1; RED_ONCE(70, "overlay: cannot create its font"); return 0; }
        VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, O.font, &rq); O.fa = vk_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); vkBindImageMemory(g_dev, O.font, O.fa.b->mem, O.fa.off);
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = O.font, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCreateImageView(g_dev, &vci, NULL, &O.fview);
        void *m; O.stage = buf_make(sizeof ovl_font, &O.sa, &m); memcpy(m, ovl_font, sizeof ovl_font);
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b }; vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &O.dsl);
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps }; vkCreateDescriptorPool(g_dev, &dpi, NULL, &O.dp);
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = O.dp, .descriptorSetCount = 1, .pSetLayouts = &O.dsl }; vkAllocateDescriptorSets(g_dev, &dai, &O.ds);
        VkDescriptorImageInfo ii = { O.samp, O.fview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = O.ds, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
        vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
        VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ovl_pc) };
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &O.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
        vkCreatePipelineLayout(g_dev, &pli, NULL, &O.pl);
    }
    VkShaderModule vm, fm;
    VkShaderModuleCreateInfo mv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof ovl_vert_spv, .pCode = ovl_vert_spv }, mf = mv; mf.codeSize = sizeof ovl_frag_spv; mf.pCode = ovl_frag_spv;
    vkCreateShaderModule(g_dev, &mv, NULL, &vm); vkCreateShaderModule(g_dev, &mf, NULL, &fm);
    VkPipelineShaderStageCreateInfo st[2] = { { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
                                              { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = O.pl };
    VkResult r = vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gi, NULL, &O.pipe);
    vkDestroyShaderModule(g_dev, vm, NULL); vkDestroyShaderModule(g_dev, fm, NULL);
    if (r) { O.failed = 1; RED_ONCE(71, "overlay: pipeline failed %d", r); return 0; }
    O.fmt = fmt; VLOG("touch overlay ready (format %d)", fmt);
    return 1;
}
// Records the overlay into cb over image img (view, size ext, format fmt), which is in TRANSFER_DST layout; leaves it in layout fin. Returns 0 (nothing recorded) when there is nothing to draw
static int ovl_draw(VkCommandBuffer cb, VkImage img, VkImageView view, VkExtent2D ext, VkFormat fmt, VkImageLayout fin) {
    dm_ovl_item it[128]; int n = front_overlay(it, 128, (int)ext.width, (int)ext.height);
    if (n <= 0 || !view || !ovl_setup(fmt)) return 0;
    if (!O.font_ready) {   // the label atlas, once, in this same command buffer
        VkImageMemoryBarrier fb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = O.font, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &fb);
        VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { ovl_font_w, ovl_font_h, 1 } };
        vkCmdCopyBufferToImage(cb, O.stage, O.font, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; fb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; fb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &fb);
        O.font_ready = 1;
    }
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { {0, 0}, ext }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = { 0, 0, (float)ext.width, (float)ext.height, 0, 1 }; VkRect2D sc = { {0, 0}, ext };
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, O.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, O.pl, 0, 1, &O.ds, 0, NULL);
    for (int i = 0; i < n; i++) {
        dm_ovl_item *q = &it[i];
        ovl_pc pc = { { q->x, q->y, q->w, q->h }, { q->r, q->g, q->b, q->a }, { (float)q->shape, q->ring, q->radius, q->h > 0 ? q->w / q->h : 1 },
                      { (float)q->glyph, q->gscale > 0 ? q->gscale : 0.5f, q->galpha, 0 }, { (float)ext.width, (float)ext.height } };
        vkCmdPushConstants(cb, O.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cb, 6, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
    ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout = fin;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    return 1;
}
#ifndef __ANDROID__
// DM_OVL_SHOT=<frame>:<path.png> (Mac Vulkan host): the game frame fitted into a phone-sized picture (2340x1080) with the touch overlay over it, as present_window composes it
void touch_ui_demo(int c, float dx, float dy);
static void ovl_shot(vtex *x, const int crop[4], const char *path) {
    VkExtent2D ext = { 2340, 1080 }; VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { ext.width, ext.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
    VkImage im; if (vkCreateImage(g_dev, &ci, NULL, &im)) return;
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, im, &rq); vmalloc_t ia = vk_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); vkBindImageMemory(g_dev, im, ia.b->mem, ia.off);
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView vw; vkCreateImageView(g_dev, &vci, NULL, &vw);
    vmalloc_t ba; void *map; VkBuffer rb = buf_make((VkDeviceSize)ext.width * ext.height * 4, &ba, &map);
    { const char *d = getenv("DM_OVL_PRESS"); touch_ui_demo(d ? atoi(d) : 2, 0, 0); }
    VkCommandBuffer cb = once_begin();
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    int32_t cx = crop ? crop[0] : 0, cy = crop ? crop[1] : 0, cw = crop && crop[2] ? crop[2] : (int32_t)x->w, ch = crop && crop[3] ? crop[3] : (int32_t)x->h;
    double sc = fmin((double)ext.width / cw, (double)ext.height / ch); int32_t dw = (int32_t)(cw * sc), dh = (int32_t)(ch * sc), dx = ((int32_t)ext.width - dw) / 2, dy = ((int32_t)ext.height - dh) / 2;
    int flip = atomic_load(&g_origin_ll);
    VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{cx, flip ? cy + ch : cy, 0}, {cx + cw, flip ? cy : cy + ch, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{dx, dy, 0}, {dx + dw, dy + dh, 1}} };
    vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    if (!ovl_draw(cb, im, vw, ext, fmt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    }
    VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { ext.width, ext.height, 1 } };
    vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bc);
    once_end(cb);
    png_write(path, map, ext.width, ext.height, 0);
    VLOG("overlay shot %s", path);
}
#endif
static void present_window(vtex *x, const int crop[4]) {
    void *win = front_metal_layer();
    if (!win) { if (W.sc) swap_destroy(); return; }
    if (!g_has_swapchain || !x || !x->img || !(x->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || x->samples > 1) return;
    int srgb = x->fmt == VK_FORMAT_R8G8B8A8_SRGB || x->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (!swap_ensure(win, srgb)) return;
    uint32_t sl = W.slot++ % 3;
    vkWaitForFences(g_dev, 1, &W.fence[sl], VK_TRUE, UINT64_MAX);
    uint32_t ii; VkResult r = vkAcquireNextImageKHR(g_dev, W.sc, UINT64_MAX, W.acq[sl], VK_NULL_HANDLE, &ii);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { swap_destroy(); return; }
    if (r && r != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(g_dev, 1, &W.fence[sl]);
    VkCommandBuffer cb = W.cb[sl]; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = W.img[ii], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, W.img[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    int32_t cx = crop ? crop[0] : 0, cy = crop ? crop[1] : 0, cw = crop && crop[2] ? crop[2] : (int32_t)x->w, ch = crop && crop[3] ? crop[3] : (int32_t)x->h;
    double sc = fmin((double)W.ext.width / cw, (double)W.ext.height / ch); int32_t dw = (int32_t)(cw * sc), dh = (int32_t)(ch * sc), dx = ((int32_t)W.ext.width - dw) / 2, dy = ((int32_t)W.ext.height - dh) / 2;
    int flip = atomic_load(&g_origin_ll);
    VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{cx, flip ? cy + ch : cy, 0}, {cx + cw, flip ? cy : cy + ch, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{dx, dy, 0}, {dx + dw, dy + dh, 1}} };
    vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, W.img[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    if (!ovl_draw(cb, W.img[ii], W.view[ii], W.ext, W.fmt, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) {
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = 0; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    }
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &W.acq[sl], .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &W.done[sl] };
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &W.done[sl], .swapchainCount = 1, .pSwapchains = &W.sc, .pImageIndices = &ii };
    pthread_mutex_lock(&g_qmu);
    r = vkQueueSubmit(g_q, 1, &si, W.fence[sl]);
    if (!r) r = vkQueuePresentKHR(g_q, &pi);
    pthread_mutex_unlock(&g_qmu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) swap_destroy();
}
// Once a minute: where the renderer's memory is, so growth can be told apart (caches that level off: shaders, pipelines; anything else that keeps rising is a leak)
static void mem_report(void) {
    pthread_mutex_lock(&g_amu); double dev = 0, devused = 0; int nb = g_nblocks;
    for (int i = 0; i < g_nblocks; i++) { dev += g_blocks[i]->size; double fr = 0; for (int k = 0; k < g_blocks[i]->nfree; k++) fr += g_blocks[i]->fr[k].size; devused += g_blocks[i]->size - fr; }
    pthread_mutex_unlock(&g_amu);
    double arena = 0; int ndp = 0; for (int i = 0; i < NSUB; i++) { for (int k = 0; k < g_subs[i].nar; k++) arena += g_subs[i].ar[k].size; ndp += g_subs[i].ndp; }
    pthread_mutex_lock(&g_dfmu); int ndf = g_ndfr; pthread_mutex_unlock(&g_dfmu);
    pthread_mutex_lock(&g_mu); int npool = g_npool; double pool = 0; for (int i = 0; i < g_npool; i++) pool += g_pools[i].size; pthread_mutex_unlock(&g_mu);
    long heap = -1;
#ifdef __ANDROID__
    { struct mallinfo mi = mallinfo(); heap = (long)mi.uordblks; }
#endif
    VLOG("[mem] device blocks %d: %.0f MB (%.0f MB in use); textures %ld live, %.0f MB; pools %d, %.0f MB; arenas %.0f MB; descriptor pools %d; deferred %d; "
         "shaders %u, pipelines %u, textures by serial %u, aliases %u, ghosts %u; host heap %.0f MB",
         nb, dev / 1048576, devused / 1048576, atomic_load(&g_live_tex), atomic_load(&g_live_tex_bytes) / 1048576.0, npool, pool / 1048576, arena / 1048576, ndp, ndf,
         g_shcache.n, g_pso.n, g_by_serial.n, g_alias.n, g_ghost.n, heap / 1048576.0);
}
// The front end's frame report (Android: performance hints, android_main.c): the presenting thread's work since its last present against the frame's period
__attribute__((weak)) void front_frame_work(uint64_t busy_ns, uint64_t target_ns) { (void)busy_ns; (void)target_ns; }   // (no front end report: nothing)
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    uint64_t n = atomic_fetch_add(&g_present_n, 1) + 1;
    static uint64_t g_present_exit;   // when the last present returned (after its pacing sleep): from there to this call is the frame's work
    if (g_present_exit && period_s > 0) front_frame_work(mono_ns() - g_present_exit, (uint64_t)(period_s * 1e9));
    static long shots[256]; static int nshot = -1; static const char *shot_path;
    if (nshot < 0) { nshot = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { shot_path = c + 1; for (const char *q = e; q < c && nshot < 256; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) shots[nshot++] = atol(q); } }
    for (int i = 0; i < nshot; i++) if ((long)n == shots[i]) { vtex *x = tex_res(t); char path[1024]; snprintf(path, sizeof path, shot_path, (int)n); if (x && x->img) shot(x, crop, path); }
    if (front_metal_layer()) present_window(tex_res(t), crop);
#ifndef __ANDROID__
    { static long of = -1; static const char *op; if (of < 0) { const char *e = getenv("DM_OVL_SHOT"), *c = e ? strchr(e, ':') : NULL; of = c ? atol(e) : 0; op = c ? c + 1 : NULL; }
      if (of && (long)n == of && op) { vtex *x = tex_res(t); if (x && x->img) ovl_shot(x, crop, op); } }
#endif
    uint64_t now = mono_ns(), due = g_last_present + (uint64_t)(period_s * 1e9);
    if (period_s > 0 && due > now) { struct timespec ts = { (time_t)((due - now) / 1000000000ull), (long)((due - now) % 1000000000ull) }; nanosleep(&ts, NULL); now = due; }
    g_last_present = now; g_present_exit = mono_ns();
    if (n % 900 == 0 && atomic_load(&g_pso_new) >= 10) { static atomic_int busy; if (!atomic_exchange(&busy, 1)) {   // new pipelines: the cache file is rewritten in the background
        pthread_t t; void *saver(void *a); if (pthread_create(&t, NULL, saver, &busy)) atomic_store(&busy, 0); else pthread_detach(t); } }
    if (n % 1800 == 0) { VLOG("pipelines built during play so far: %d", atomic_load(&g_pso_ingame)); mem_report(); }
    if (n == 1 || n % 300 == 0) VLOG("present %llu: %llu render target sets (%llu kept the open pass, %llu passes begun), %llu draws so far; textures uploaded %llu (re-uploaded %llu, %.1f MB)", (unsigned long long)n, (unsigned long long)g_stat_passes, (unsigned long long)g_stat_rt_same, (unsigned long long)g_stat_begins,
                                     (unsigned long long)g_stat_draws, (unsigned long long)g_pkg_uploads, (unsigned long long)g_pkg_reuploads, g_up_bytes / 1048576.0);
    return n;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 60; }
__attribute__((weak)) int nvn_game_fps(void) { return 60; }
int mtl_fps_target(void) { const char *e = getenv("DM_FPS"); int f = e ? atoi(e) : 0; return f == 30 || f == 60 ? f : nvn_game_fps() == 30 ? 30 : 60; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_set_frame(uint64_t f) { g_frame_no = f; }
double mtl_gpu_ms(uint64_t f) { unsigned k = f % GRING; if (!g_tsq || atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending) > 0) return -1; return atomic_load(&g_gpu[k].ns) / 1e6; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
// statistics read by the host's report lines
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_passes; *draws = g_stat_draws; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *n) { *a = *b = 0; *n = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_create_ns(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_pkg_stats(uint64_t *u, uint64_t *r) { *u = g_pkg_uploads; *r = g_pkg_reuploads; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return g_up_bytes; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) { (void)frame; (void)rss_mb; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "Vulkan: no residency set"); }
void mtl_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { *deferred = *conflicts = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = *full = 0; }
uint64_t mtl_blit_splits(void) { return 0; }
uint64_t mtl_cut_count(void) { return 0; }
