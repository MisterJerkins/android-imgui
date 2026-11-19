#include "Graphics/Vulkan/VulkanLoader.h"

#include "Utils/System.h"

#include <format>
#include <utility>

Result<> VulkanInstanceApi::Load(PFN_vkGetInstanceProcAddr get, VkInstance instance) {
    const char* missing = nullptr;
    const auto find = [&]<class F>(F& slot, const char* name) {
        slot = reinterpret_cast<F>(get(instance, name));
        if (!slot && !missing) {
            missing = name;
        }
    };
    find(destroyInstance, "vkDestroyInstance");
    find(enumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    find(getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    find(getPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    find(getPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    find(createDevice, "vkCreateDevice");
    find(getDeviceProcAddr, "vkGetDeviceProcAddr");

    find(createAndroidSurface, "vkCreateAndroidSurfaceKHR");
    find(destroySurface, "vkDestroySurfaceKHR");
    find(getPhysicalDeviceSurfaceSupport, "vkGetPhysicalDeviceSurfaceSupportKHR");
    find(getPhysicalDeviceSurfaceCapabilities, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    find(getPhysicalDeviceSurfaceFormats, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (missing) {
        return Err("Vulkan here has no {}", missing);
    }
    return {};
}

Result<> VulkanDeviceApi::Load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
    const char* missing = nullptr;
    const auto find = [&]<class F>(F& slot, const char* name) {
        slot = reinterpret_cast<F>(get(device, name));
        if (!slot && !missing) {
            missing = name;
        }
    };
    find(destroyDevice, "vkDestroyDevice");
    find(getDeviceQueue, "vkGetDeviceQueue");
    find(deviceWaitIdle, "vkDeviceWaitIdle");
    find(queueSubmit, "vkQueueSubmit");
    find(queueWaitIdle, "vkQueueWaitIdle");

    find(createCommandPool, "vkCreateCommandPool");
    find(destroyCommandPool, "vkDestroyCommandPool");
    find(allocateCommandBuffers, "vkAllocateCommandBuffers");
    find(beginCommandBuffer, "vkBeginCommandBuffer");
    find(endCommandBuffer, "vkEndCommandBuffer");
    find(createSemaphore, "vkCreateSemaphore");
    find(destroySemaphore, "vkDestroySemaphore");
    find(createFence, "vkCreateFence");
    find(destroyFence, "vkDestroyFence");
    find(resetFences, "vkResetFences");
    find(waitForFences, "vkWaitForFences");

    find(createDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    find(destroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    find(createPipelineLayout, "vkCreatePipelineLayout");
    find(destroyPipelineLayout, "vkDestroyPipelineLayout");
    find(createDescriptorPool, "vkCreateDescriptorPool");
    find(destroyDescriptorPool, "vkDestroyDescriptorPool");
    find(allocateDescriptorSets, "vkAllocateDescriptorSets");
    find(freeDescriptorSets, "vkFreeDescriptorSets");
    find(updateDescriptorSets, "vkUpdateDescriptorSets");
    find(createRenderPass, "vkCreateRenderPass");
    find(destroyRenderPass, "vkDestroyRenderPass");
    find(createShaderModule, "vkCreateShaderModule");
    find(destroyShaderModule, "vkDestroyShaderModule");
    find(createGraphicsPipelines, "vkCreateGraphicsPipelines");
    find(destroyPipeline, "vkDestroyPipeline");
    find(createFramebuffer, "vkCreateFramebuffer");
    find(destroyFramebuffer, "vkDestroyFramebuffer");

    find(allocateMemory, "vkAllocateMemory");
    find(freeMemory, "vkFreeMemory");
    find(mapMemory, "vkMapMemory");
    find(createBuffer, "vkCreateBuffer");
    find(destroyBuffer, "vkDestroyBuffer");
    find(getBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    find(bindBufferMemory, "vkBindBufferMemory");
    find(createImage, "vkCreateImage");
    find(destroyImage, "vkDestroyImage");
    find(getImageMemoryRequirements, "vkGetImageMemoryRequirements");
    find(bindImageMemory, "vkBindImageMemory");
    find(createImageView, "vkCreateImageView");
    find(destroyImageView, "vkDestroyImageView");
    find(createSampler, "vkCreateSampler");
    find(destroySampler, "vkDestroySampler");

    find(cmdPipelineBarrier, "vkCmdPipelineBarrier");
    find(cmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    find(cmdBeginRenderPass, "vkCmdBeginRenderPass");
    find(cmdEndRenderPass, "vkCmdEndRenderPass");
    find(cmdBindPipeline, "vkCmdBindPipeline");
    find(cmdSetViewport, "vkCmdSetViewport");
    find(cmdSetScissor, "vkCmdSetScissor");
    find(cmdPushConstants, "vkCmdPushConstants");
    find(cmdBindVertexBuffers, "vkCmdBindVertexBuffers");
    find(cmdBindIndexBuffer, "vkCmdBindIndexBuffer");
    find(cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    find(cmdDrawIndexed, "vkCmdDrawIndexed");

    find(createSwapchain, "vkCreateSwapchainKHR");
    find(destroySwapchain, "vkDestroySwapchainKHR");
    find(getSwapchainImages, "vkGetSwapchainImagesKHR");
    find(acquireNextImage, "vkAcquireNextImageKHR");
    find(queuePresent, "vkQueuePresentKHR");
    if (missing) {
        return Err("the Vulkan device has no {}", missing);
    }
    return {};
}

const char* VulkanLoader::NameOf(VkResult code) {
    switch (code) {
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_INCOMPLETE:
            return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:
            return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY:
            return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        default:
            return nullptr;
    }
}

std::string VulkanLoader::Describe(VkResult code) {
    const char* name = NameOf(code);
    return name ? name : std::format("VkResult {}", static_cast<int>(code));
}

Result<> VulkanLoader::Check(const char* what, VkResult code) {
    if (code != VK_SUCCESS) {
        return Err("{} failed: {}", what, VulkanLoader::Describe(code));
    }
    return {};
}

Result<VulkanInstance> VulkanInstance::Create(const VkInstanceCreateInfo& info) {
    const auto library = Library::Open("libvulkan.so");
    if (!library) {
        return Err("Vulkan is not available: {}", library.error().message);
    }
    const auto get = library->Func<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto create = get ? reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance")) : nullptr;
    if (!create) {
        return Err("Vulkan is not available: libvulkan.so has no vkCreateInstance");
    }

    VulkanInstance instance;
    TRY(VulkanLoader::Check("vkCreateInstance", create(&info, nullptr, &instance.handle_)));
    TRY(instance.api_.Load(get, instance.handle_));
    return instance;
}

VulkanInstance::VulkanInstance(VulkanInstance&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)), api_(other.api_) {
}

VulkanInstance::~VulkanInstance() {
    if (handle_ && api_.destroyInstance) {
        api_.destroyInstance(handle_, nullptr);
    }
}

Result<VulkanDevice> VulkanDevice::Create(const VulkanInstance& instance, VkPhysicalDevice physical, const VkDeviceCreateInfo& info) {
    VulkanDevice device;
    TRY(VulkanLoader::Check("vkCreateDevice", instance->createDevice(physical, &info, nullptr, &device.handle_)));
    TRY(device.api_.Load(instance->getDeviceProcAddr, device.handle_));
    return device;
}

VulkanDevice::VulkanDevice(VulkanDevice&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)), api_(other.api_) {
}

VulkanDevice::~VulkanDevice() {
    if (handle_ && api_.destroyDevice) {
        api_.destroyDevice(handle_, nullptr);
    }
}
