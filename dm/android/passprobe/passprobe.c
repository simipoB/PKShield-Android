// Pass replay probe (no game data): makes the render targets Pokémon Sword / Shield makes in its first frames (passes.h, from a pass log of the engine,
// gen_passes.py) the way nvn_vk.c makes them on Arm's driver (usage from the format's features, mutable with a view format list, cube compatible like tex_make),
// and begins / ends the game's passes in the game's order, frame by frame, with the swapchain passes on a real swapchain. Each pass is logged (logcat tag
// passprobe) before the driver sees it, so a crash inside the driver's pass begin (Arm's r44p1: SIGSEGV at 0x90 in libGLES_mali.so, frame 3) names its pass.
// Tests (a number in <files>/variant, written by MainActivity): 0 the game's passes + its per-frame clears and buffer copies, 1 + the engine's
//   present (clear, blit), 2 + the touch overlay (label atlas upload, a pass on the screen image). Each command is logged before the driver sees it. Results: <files>/run-<variant>.txt (MainActivity shows and shares them) and logcat tag passprobe.
#include <android/log.h>
#include <fcntl.h>
#include <stdarg.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

static int g_out = -1;   // the run's result file (<files>/run-<variant>.txt): each line written before the driver call it names, so a crash keeps it
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return; if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    __android_log_write(ANDROID_LOG_INFO, "passprobe", b);
    if (g_out >= 0) { b[n++] = '\n'; (void)!write(g_out, b, (size_t)n); }
}
#define LOG(...) logf_(__VA_ARGS__)
#define CHECK(x) do { VkResult r_ = (x); if (r_) { LOG("FAIL %s: %d", #x, r_); return; } } while (0)

typedef struct { uint32_t w, h, d, fmt, levels, layers, samples, usage, is3d, cube; } pp_img;
typedef struct { int img; uint32_t fmt, base_level, base_layer, layers; } pp_view;
typedef struct { int frame, sc; uint32_t w, h, layers; int ncol; int col[8]; int load[8]; int dview, hasd, hass, site; } pp_pass;
#include "passes.h"
#define NIMG (int)(sizeof PP_IMGS / sizeof PP_IMGS[0])
#define NVIEW (int)(sizeof PP_VIEWS / sizeof PP_VIEWS[0])
#define NPASS (int)(sizeof PP_PASSES / sizeof PP_PASSES[0])

static ANativeActivity *g_act; static ANativeWindow *g_win; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_variant;   // the test: 0, 1 or 2 (see the top)
static VkInstance inst; static VkPhysicalDevice pd; static VkDevice dev; static VkQueue q; static uint32_t qf; static VkPhysicalDeviceMemoryProperties mp;
static VkImage img[256]; static VkFormat imgfmt[256]; static VkImageView view[256];
static int g_dynren; static PFN_vkCmdBeginRendering p_begin; static PFN_vkCmdEndRendering p_end;
static VkRenderPass g_rp[512]; static VkFramebuffer g_fb[512]; static int g_nrp;   // this frame's (pass_free after its fence)
// A pass described as dynamic rendering; without it (Vulkan 1.1) a render pass and framebuffer with the same attachments, layouts and load / store ops
// (as nvn_vk.c rend_begin). cf: each colour attachment's format; df: the depth / stencil attachment's; samples: the attachments' sample count
static void pass_begin(VkCommandBuffer cb, const VkRenderingInfo *ri, const VkFormat *cf, VkFormat df, uint32_t samples) {
    if (g_dynren) { p_begin(cb, ri); return; }
    VkAttachmentDescription ad[9]; VkAttachmentReference cr[8], dr; VkImageView v[9]; VkClearValue cv[9]; uint32_t n = 0;
    VkSampleCountFlagBits sc = (VkSampleCountFlagBits)(samples ? samples : 1);
    for (uint32_t i = 0; i < ri->colorAttachmentCount && i < 8; i++) {
        const VkRenderingAttachmentInfo *c = &ri->pColorAttachments[i];
        if (!c->imageView) { cr[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED }; continue; }
        ad[n] = (VkAttachmentDescription){ 0, cf[i], sc, c->loadOp, c->storeOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, c->imageLayout, c->imageLayout };
        cr[i] = (VkAttachmentReference){ n, c->imageLayout }; memset(&cv[n], 0, sizeof cv[n]); v[n++] = c->imageView;
    }
    const VkRenderingAttachmentInfo *d = ri->pDepthAttachment && ri->pDepthAttachment->imageView ? ri->pDepthAttachment : ri->pStencilAttachment && ri->pStencilAttachment->imageView ? ri->pStencilAttachment : NULL;
    if (d) { ad[n] = (VkAttachmentDescription){ 0, df, sc, d->loadOp, d->storeOp, d->loadOp, d->storeOp, d->imageLayout, d->imageLayout };
        dr = (VkAttachmentReference){ n, d->imageLayout }; memset(&cv[n], 0, sizeof cv[n]); v[n++] = d->imageView; }
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = ri->colorAttachmentCount, .pColorAttachments = ri->colorAttachmentCount ? cr : NULL,
        .pDepthStencilAttachment = d ? &dr : NULL };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = n, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp };
    VkRenderPass rp; if (vkCreateRenderPass(dev, &rci, NULL, &rp)) { LOG("FAIL vkCreateRenderPass"); return; }
    uint32_t w = ri->renderArea.offset.x + ri->renderArea.extent.width, h = ri->renderArea.offset.y + ri->renderArea.extent.height;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = n, .pAttachments = v, .width = w ? w : 1, .height = h ? h : 1,
        .layers = ri->layerCount ? ri->layerCount : 1 };
    VkFramebuffer fb; if (vkCreateFramebuffer(dev, &fci, NULL, &fb)) { LOG("FAIL vkCreateFramebuffer"); return; }
    if (g_nrp < 512) { g_rp[g_nrp] = rp; g_fb[g_nrp++] = fb; }
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = ri->renderArea, .clearValueCount = n, .pClearValues = cv };
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}
static void pass_end(VkCommandBuffer cb) { if (g_dynren) p_end(cb); else vkCmdEndRenderPass(cb); }
static void pass_free(void) { for (int i = 0; i < g_nrp; i++) { vkDestroyFramebuffer(dev, g_fb[i], NULL); vkDestroyRenderPass(dev, g_rp[i], NULL); } g_nrp = 0; }

static int is_depth(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_S8_UINT ||
    f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static VkImageAspectFlags aspect(VkFormat f) {
    if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
    return is_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}
static VkFormat twin_of(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM :
           f == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f;
}
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (bits & (1u << i)) return i;
    return 0;
}

static int make_image(int i) {
    const pp_img *m = &PP_IMGS[i]; VkFormat f = (VkFormat)m->fmt;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
    if (is_depth(f) && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {   // (the engine maps a depth format the GPU lacks the same way)
        VkFormat alt = f == VK_FORMAT_D24_UNORM_S8_UINT ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
        LOG("image %d: format %d has no depth attachment support here: %d instead", i, f, alt); f = alt; vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
    }
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = m->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = f,
        .extent = { m->w, m->h, m->is3d ? m->d : 1 }, .mipLevels = m->levels, .arrayLayers = m->is3d ? 1 : m->layers, .samples = (VkSampleCountFlagBits)m->samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT };
    if (m->is3d) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    if (!m->is3d && ci.arrayLayers >= 6 && m->w == m->h && m->samples == 1) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((m->usage & VK_IMAGE_USAGE_STORAGE_BIT) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkFormat tw = twin_of(f), fl[2] = { f, tw };
    VkImageFormatListCreateInfo fli = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = tw != f ? 2 : 1, .pViewFormats = fl };
    if (!m->is3d) {   // nvn_vk.c tex_make on Arm's driver
        if (is_depth(f)) ci.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        else if (tw == f) ci.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        else ci.pNext = &fli;
    }
    VkResult r = vkCreateImage(dev, &ci, NULL, &img[i]);
    if (r) { LOG("image %d: vkCreateImage %ux%ux%u format %d levels %u layers %u usage %#x flags %#x failed: %d", i, m->w, m->h, m->d, f, m->levels, m->layers, ci.usage, ci.flags, r); return 0; }
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev, img[i], &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory mem; if (vkAllocateMemory(dev, &ai, NULL, &mem) || vkBindImageMemory(dev, img[i], mem, 0)) { LOG("image %d: memory failed", i); return 0; }
    imgfmt[i] = f;
    return 1;
}
static int make_view(int i) {
    const pp_view *v = &PP_VIEWS[i]; const pp_img *m = &PP_IMGS[v->img]; VkFormat f = imgfmt[v->img];
    VkFormat vf = is_depth(f) ? f : (VkFormat)v->fmt;
    VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[v->img], .format = vf,
        .viewType = v->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .subresourceRange = { aspect(f), v->base_level, 1, v->base_layer, v->layers } };
    (void)m;
    VkResult r = vkCreateImageView(dev, &ci, NULL, &view[i]);
    if (r) { LOG("view %d (image %d): vkCreateImageView format %d level %u layer %u+%u failed: %d", i, v->img, vf, v->base_level, v->base_layer, v->layers, r); return 0; }
    return 1;
}

static void run(ANativeWindow *win) {
    LOG("start: variant %d, %d images, %d views, %d passes", g_variant, NIMG, NVIEW, NPASS);
    const char *iext[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3, .pApplicationName = "passprobe" };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); if (!n) { LOG("FAIL no GPU"); return; }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd, &pr); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    LOG("GPU %s vendor %#x driver %#x (r%up%u for Arm) API %u.%u.%u", pr.deviceName, pr.vendorID, pr.driverVersion, pr.driverVersion >> 22, (pr.driverVersion >> 12) & 0x3ff,
        VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion), VK_API_VERSION_PATCH(pr.apiVersion));
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qf = 0; qf < nq && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); qf++) {}
    float pri = 1; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &pri };
    // dynamic rendering where the GPU has Vulkan 1.3 (as the engine), else render passes and framebuffers (the engine's path on Vulkan 1.1 drivers, Mali r32 and older)
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    if (VK_API_VERSION_MINOR(pr.apiVersion) >= 3) { VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 }; vkGetPhysicalDeviceFeatures2(pd, &f2); }
    g_dynren = f13.dynamicRendering == VK_TRUE;
    f13 = (VkPhysicalDeviceVulkan13Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    const char *dext[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = g_dynren ? &f13 : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, qf, 0, &q);
    if (g_dynren) { p_begin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRendering"); p_end = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRendering"); }
    if (!p_begin || !p_end) g_dynren = 0;
    LOG("passes: %s", g_dynren ? "dynamic rendering (Vulkan 1.3)" : "render passes and framebuffers (no Vulkan 1.3 dynamic rendering here)");
    // the swapchain, as the engine's (8 images, mailbox when there is one)
    VkAndroidSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = win }; VkSurfaceKHR surf; CHECK(vkCreateAndroidSurfaceKHR(inst, &sci, NULL, &surf));
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, sf);
    VkSurfaceFormatKHR fmt = sf[0]; for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_SRGB) fmt = sf[i];
    uint32_t npm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, NULL); VkPresentModeKHR pm[8], mode = VK_PRESENT_MODE_FIFO_KHR; if (npm > 8) npm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, pm); for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pm[i];
    uint32_t want = 8; if (want < caps.minImageCount) want = caps.minImageCount; if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkExtent2D ext = caps.currentExtent; if (ext.width == 0xFFFFFFFFu) { ext.width = 1920; ext.height = 1080; }
    VkSwapchainCreateInfoKHR swci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = want, .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE };
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkSwapchainKHR sw; CHECK(vkCreateSwapchainKHR(dev, &swci, NULL, &sw));
    uint32_t nsi = 0; vkGetSwapchainImagesKHR(dev, sw, &nsi, NULL); VkImage si[16]; if (nsi > 16) nsi = 16; vkGetSwapchainImagesKHR(dev, sw, &nsi, si); VkImageView sv[16];
    for (uint32_t i = 0; i < nsi; i++) { VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = si[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }; CHECK(vkCreateImageView(dev, &vi, NULL, &sv[i])); }
    LOG("swapchain %ux%u, %u images, format %d, present mode %d", ext.width, ext.height, nsi, fmt.format, mode);
    // the game's render targets, all in GENERAL (as the engine keeps them)
    for (int i = 0; i < NIMG; i++) if (!make_image(i)) return;
    for (int i = 0; i < NVIEW; i++) if (!make_view(i)) return;
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSemaphoreCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; VkSemaphore acq, done; vkCreateSemaphore(dev, &smi, NULL, &acq); vkCreateSemaphore(dev, &smi, NULL, &done);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(cb, &bi));
    for (int i = 0; i < NIMG; i++) {
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i],
            .subresourceRange = { aspect(imgfmt[i]), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    }
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si0 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }; CHECK(vkQueueSubmit(q, 1, &si0, fence));
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &fence); pass_free();
    LOG("made %d images and %d views", NIMG, NVIEW);
    // buffers for the per-frame copies (the engine's pool mirror updates, uniform copies) and the overlay's font upload
    VkBuffer buf[3]; VkDeviceMemory bmem[3];
    for (int i = 0; i < 3; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1 << 20, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        CHECK(vkCreateBuffer(dev, &bci, NULL, &buf[i])); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, buf[i], &rq);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        CHECK(vkAllocateMemory(dev, &ai, NULL, &bmem[i])); CHECK(vkBindBufferMemory(dev, buf[i], bmem[i], 0));
    }
    // the overlay's label atlas, as nvn_vk.c ovl_setup makes it (R8, sampled + transfer destination)
    VkImageCreateInfo fci2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8_UNORM, .extent = { 512, 256, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VkImage font; CHECK(vkCreateImage(dev, &fci2, NULL, &font));
    { VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev, font, &rq);
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
      VkDeviceMemory fm; CHECK(vkAllocateMemory(dev, &ai, NULL, &fm)); CHECK(vkBindImageMemory(dev, font, fm, 0)); }
    int font_ready = 0;
    // the picture the engine presents: the game's largest 8-bit RGBA target (its final 1920x1080 picture), else its largest 2D colour target
    int scene = -1;
    for (int pass8 = 1; pass8 >= 0 && scene < 0; pass8--)
        for (int i = 0; i < NIMG; i++) {
            VkFormat f = imgfmt[i]; int rgba8 = f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
            if (is_depth(f) || PP_IMGS[i].is3d || (pass8 && !rgba8)) continue;
            if (scene < 0 || PP_IMGS[i].w * PP_IMGS[i].h > PP_IMGS[scene].w * PP_IMGS[scene].h) scene = i;
        }
    VkFormatProperties sfp; vkGetPhysicalDeviceFormatProperties(pd, imgfmt[scene], &sfp);
    VkFilter filt = sfp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    LOG("test %d: %s; the presented picture: img%d %ux%u format %d", g_variant + 1, g_variant == 0 ? "the game's passes, clears and buffer copies" :
        g_variant == 1 ? "1 + the engine's present (clear to black, blit, present)" : "2 + the touch overlay (label atlas upload, a pass on the screen image)", scene, PP_IMGS[scene].w, PP_IMGS[scene].h, imgfmt[scene]);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    int nframes = PP_PASSES[NPASS - 1].frame + 1, total = 0, step = 0;
#define CMD(what, ...) do { LOG("round %d frame %d step %d: %s", round, fr, step++, what); __VA_ARGS__; } while (0)
#define FULLBAR() vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL)
    for (int round = 0; round < 3; round++) for (int fr = 0; fr < nframes; fr++) {
        uint32_t ii = 0; VkResult ar = vkAcquireNextImageKHR(dev, sw, UINT64_MAX, acq, VK_NULL_HANDLE, &ii); if (ar && ar != VK_SUBOPTIMAL_KHR) { LOG("FAIL acquire: %d", ar); return; }
        vkResetCommandBuffer(cb, 0); CHECK(vkBeginCommandBuffer(cb, &bi));
        // the game's frame: buffer copies (pool mirrors, uniforms), clears of its targets in GENERAL, its passes
        { VkBufferCopy c = { 0, 0, 132 * 1024 }; CMD("vkCmdCopyBuffer (pool mirror)", vkCmdCopyBuffer(cb, buf[0], buf[1], 1, &c)); }
        CMD("memory barrier", FULLBAR());
        { VkBufferCopy c = { 4096, 8192, 4096 }; CMD("vkCmdCopyBuffer (uniforms)", vkCmdCopyBuffer(cb, buf[1], buf[0], 1, &c)); }
        CMD("vkCmdFillBuffer", vkCmdFillBuffer(cb, buf[1], 0, 65536, 0));
        { int nc = 0, nd = 0;
          for (int p = 0; p < NPASS; p++) {
            const pp_pass *P = &PP_PASSES[p]; if (P->frame != fr || P->sc) continue;
            for (int c = 0; c < P->ncol && nc < 13; c++) if (P->col[c] >= 0) {
                const pp_view *v = &PP_VIEWS[P->col[c]]; int im = v->img; char w[112];
                snprintf(w, sizeof w, "vkCmdClearColorImage img%d %ux%u format %d level %u", im, PP_IMGS[im].w, PP_IMGS[im].h, imgfmt[im], v->base_level);
                VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, v->base_level, 1, v->base_layer, v->layers }; VkClearColorValue cv = {{0}};
                CMD(w, FULLBAR(); vkCmdClearColorImage(cb, img[im], VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r)); nc++;
            }
            if (P->dview >= 0 && nd < 11) {
                int im = PP_VIEWS[P->dview].img; char w[112];
                snprintf(w, sizeof w, "vkCmdClearDepthStencilImage img%d %ux%u format %d", im, PP_IMGS[im].w, PP_IMGS[im].h, imgfmt[im]);
                VkImageSubresourceRange r = { aspect(imgfmt[im]), 0, 1, 0, 1 }; VkClearDepthStencilValue dv = { 1, 0 };
                CMD(w, FULLBAR(); vkCmdClearDepthStencilImage(cb, img[im], VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &r)); nd++;
            } } }
        for (int p = 0; p < NPASS; p++) {
            const pp_pass *P = &PP_PASSES[p]; if (P->frame != fr || P->sc) continue;   // (the screen passes: the present below)
            VkRenderingAttachmentInfo ca[8]; VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            for (int c = 0; c < P->ncol && c < 8; c++) {
                ca[c] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
                if (P->col[c] >= 0) { ca[c].imageView = view[P->col[c]]; ca[c].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    ca[c].loadOp = P->load[c] == 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : P->load[c] == 2 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD; }
            }
            if (P->dview >= 0) { da.imageView = view[P->dview]; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
            VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { {0, 0}, { P->w, P->h } }, .layerCount = P->layers ? P->layers : 1,
                .colorAttachmentCount = (uint32_t)P->ncol, .pColorAttachments = ca, .pDepthAttachment = P->hasd && P->dview >= 0 ? &da : NULL, .pStencilAttachment = P->hass && P->dview >= 0 ? &da : NULL };
            char w[128]; snprintf(w, sizeof w, "game pass %d %ux%u colours %d depth %s%d", p, P->w, P->h, P->ncol, P->dview >= 0 ? "img" : "-", P->dview >= 0 ? PP_VIEWS[P->dview].img : 0);
            VkFormat cf[8] = { 0 }; uint32_t smp = 1;
            for (int c = 0; c < P->ncol && c < 8; c++) if (P->col[c] >= 0) { const pp_view *v = &PP_VIEWS[P->col[c]]; cf[c] = is_depth(imgfmt[v->img]) ? imgfmt[v->img] : (VkFormat)v->fmt; smp = PP_IMGS[v->img].samples; }
            VkFormat df = P->dview >= 0 ? imgfmt[PP_VIEWS[P->dview].img] : VK_FORMAT_UNDEFINED; if (P->dview >= 0) smp = PP_IMGS[PP_VIEWS[P->dview].img].samples;
            CMD(w, FULLBAR(); pass_begin(cb, &ri, cf, df, smp); pass_end(cb)); total++;
        }
        // the present (nvn_vk.c mtl_present / scale_draw / ovl_draw)
        VkImageMemoryBarrier sb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = si[ii],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CMD("memory barrier before the present", FULLBAR());
        sb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; sb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        CMD("screen image UNDEFINED -> TRANSFER_DST", vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &sb));
        if (g_variant >= 1) {
            VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            CMD("vkCmdClearColorImage on the screen image (TRANSFER_DST)", vkCmdClearColorImage(cb, si[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr));
            VkMemoryBarrier tb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
            CMD("transfer barrier", vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tb, 0, NULL, 0, NULL));
            int32_t sw_ = (int32_t)PP_IMGS[scene].w, sh_ = (int32_t)PP_IMGS[scene].h, dw = (int32_t)ext.width, dh = (int32_t)ext.height;
            VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, sh_, 0}, {sw_, 0, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {dw, dh, 1}} };
            CMD("vkCmdBlitImage game picture (GENERAL) -> screen image (TRANSFER_DST)", vkCmdBlitImage(cb, img[scene], VK_IMAGE_LAYOUT_GENERAL, si[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, filt));
        }
        if (g_variant >= 2) {   // the touch overlay over it (ovl_draw)
            if (!font_ready) {
                VkImageMemoryBarrier fb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = font, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                CMD("label atlas UNDEFINED -> TRANSFER_DST", vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &fb));
                VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 512, 256, 1 } };
                CMD("vkCmdCopyBufferToImage label atlas", vkCmdCopyBufferToImage(cb, buf[2], font, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc));
                fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; fb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; fb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                CMD("label atlas TRANSFER_DST -> SHADER_READ_ONLY", vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &fb));
                font_ready = 1;
            }
            sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; sb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            CMD("screen image TRANSFER_DST -> COLOR_ATTACHMENT", vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &sb));
            VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = sv[ii], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
            VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { {0, 0}, ext }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
            CMD("overlay pass on the screen image", pass_begin(cb, &ri, &fmt.format, VK_FORMAT_UNDEFINED, 1); pass_end(cb));
            sb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; sb.dstAccessMask = 0; sb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            CMD("screen image COLOR_ATTACHMENT -> PRESENT_SRC", vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &sb));
        } else {
            sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; sb.dstAccessMask = 0; sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            CMD("screen image TRANSFER_DST -> PRESENT_SRC", vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &sb));
        }
        CMD("vkEndCommandBuffer", CHECK(vkEndCommandBuffer(cb)));
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo s = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &done };
        CMD("vkQueueSubmit", CHECK(vkQueueSubmit(q, 1, &s, fence)));
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &sw, .pImageIndices = &ii };
        CMD("vkQueuePresentKHR", vkQueuePresentKHR(q, &pi));
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &fence); pass_free();
    }
    LOG("DONE: %d frames x 3 rounds, %d game passes, %d steps, no crash (test %d)", nframes, total, step, g_variant + 1);
}

static void *thread_main(void *a) {
    (void)a;
    pthread_mutex_lock(&g_mu); while (!g_win) pthread_cond_wait(&g_cv, &g_mu); ANativeWindow *w = g_win; pthread_mutex_unlock(&g_mu);
    run(w);
    LOG("END");
    if (g_out >= 0) close(g_out);
    _exit(0);   // each test in a process of its own (Android keeps a finished activity's process for the next start, where this would not run again)
    return NULL;
}
static void on_window(ANativeActivity *a, ANativeWindow *w) { (void)a; pthread_mutex_lock(&g_mu); g_win = w; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
static void on_window_gone(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; }
void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t n) {
    (void)saved; (void)n;
    g_act = a;
    a->callbacks->onNativeWindowCreated = on_window; a->callbacks->onNativeWindowDestroyed = on_window_gone;
    static int started; if (started++) return;   // (one test per process: thread_main ends it)
    char path[512]; snprintf(path, sizeof path, "%s/variant", a->internalDataPath);   // written by the app's screen (MainActivity) before it starts a test
    FILE *f = fopen(path, "r"); int fv = 0; if (f) { if (fscanf(f, "%d", &fv) != 1) fv = 0; fclose(f); }
    g_variant = fv;
    snprintf(path, sizeof path, "%s/run-%d.txt", a->internalDataPath, g_variant); g_out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pthread_t t; pthread_create(&t, NULL, thread_main, NULL); pthread_detach(t);
}
