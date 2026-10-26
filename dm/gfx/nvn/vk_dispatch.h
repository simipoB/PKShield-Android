// The Vulkan functions nvn_vk.c calls, loaded at run time from the driver it opened (vk_open in nvn_vk.c): the system's libvulkan, or a custom driver
// (Mesa Turnip and other Adreno builds) opened through libadrenotools. nvn_vk.c is built with VK_NO_PROTOTYPES, so each name below is a function pointer.
// VK_GLOBAL: no instance needed; VK_INSTANCE: loaded once the instance exists; VK_DEVICE: once the device exists (vkGetDeviceProcAddr, no dispatch overhead)
#pragma once
#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_DEVICE(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueuePresentKHR) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkBindBufferMemory) X(vkBindImageMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetImageMemoryRequirements) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreatePipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkCreateSemaphore) X(vkWaitSemaphores) \
    X(vkCreateQueryPool) X(vkGetQueryPoolResults) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) X(vkCmdBindVertexBuffers2) \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdResolveImage) \
    X(vkCmdFillBuffer) X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBias) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp)
#define VK_DECLARE(f) static PFN_##f f;
VK_GLOBAL(VK_DECLARE) VK_INSTANCE(VK_DECLARE) VK_DEVICE(VK_DECLARE)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#undef VK_DECLARE
