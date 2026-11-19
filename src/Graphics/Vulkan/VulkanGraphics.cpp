#include "Graphics/Vulkan/VulkanGraphics.h"

#include "Utils/Logger.h"
#include "Window/ANativeWindowCreator.h"

#include "imgui.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <utility>

constexpr uint32_t MostTextures = 1024;
constexpr VkFormat TextureFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageSubresourceRange WholeImage{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

Result<VulkanBase> VulkanBase::Create(ANativeWindow* window) {
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_0;
    const char* const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
    info.ppEnabledExtensionNames = extensions;

    VulkanBase base(TRY(VulkanInstance::Create(info)));
    TRY(base.Attach(window));
    return base;
}

VulkanBase::VulkanBase(VulkanBase&& other) noexcept : instance_(std::move(other.instance_)), surface_(std::exchange(other.surface_, {})), window_(std::exchange(other.window_, nullptr)) {
}

VulkanBase::~VulkanBase() {
    if (surface_) {
        instance_->destroySurface(instance_, surface_, nullptr);
    }
}

Result<> VulkanBase::Attach(ANativeWindow* window) {
    instance_->destroySurface(instance_, std::exchange(surface_, {}), nullptr);
    window_ = nullptr;
    VkAndroidSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = window;
    TRY(VulkanLoader::Check("vkCreateAndroidSurfaceKHR", instance_->createAndroidSurface(instance_, &info, nullptr, &surface_)));
    window_ = window;
    return {};
}

// What the driver hands out in two calls: how many there are, then the items themselves.
template <class T, class Call>
Result<std::vector<T>> VulkanGraphics::Enumerate(const char* what, Call call) {
    for (;;) {
        uint32_t count = 0;
        TRY(VulkanLoader::Check(what, call(&count, nullptr)));
        std::vector<T> items(count);
        const VkResult code = call(&count, items.data());
        if (code == VK_INCOMPLETE) {
            continue;
        }
        TRY(VulkanLoader::Check(what, code));
        items.resize(count);
        return items;
    }
}

struct VulkanGraphics::Choice {
    VkPhysicalDevice physical;
    uint32_t family;
};

// The first device with a queue that both draws and shows its pictures in the window.
Result<VulkanGraphics::Choice> VulkanGraphics::ChooseDevice(const VulkanBase& base) {
    const VulkanInstance& instance = base.GetInstance();
    const auto devices = TRY(Enumerate<VkPhysicalDevice>("vkEnumeratePhysicalDevices", [&](uint32_t* count, VkPhysicalDevice* items) { return instance->enumeratePhysicalDevices(instance, count, items); }));
    for (const VkPhysicalDevice physical : devices) {
        uint32_t count = 0;
        instance->getPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        instance->getPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        for (uint32_t family = 0; family < count; family++) {
            VkBool32 presents = VK_FALSE;
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            if (instance->getPhysicalDeviceSurfaceSupport(physical, family, base.GetSurface(), &presents) == VK_SUCCESS && presents) {
                return Choice{physical, family};
            }
        }
    }
    return Err("no Vulkan device can draw to the overlay's window");
}

Result<std::unique_ptr<Graphics>> VulkanGraphics::Create(const NativeWindow& window) {
    VulkanBase base = TRY(VulkanBase::Create(window.Get()));
    const auto [physical, family] = TRY(ChooseDevice(base));

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* const extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
    info.ppEnabledExtensionNames = extensions;
    VulkanDevice device = TRY(VulkanDevice::Create(base.GetInstance(), physical, info));
    VkPhysicalDeviceProperties properties{};
    base.GetInstance()->getPhysicalDeviceProperties(physical, &properties);

    std::unique_ptr<VulkanGraphics> graphics(new VulkanGraphics(window, std::move(base), std::move(device), physical, family, properties.limits.maxImageDimension2D));
    TRY(graphics->Prepare());
    TRY(graphics->RebuildTarget());

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "android-imgui Vulkan";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = static_cast<int>(graphics->maxTextureSide_);
    logger.Info("Vulkan {}.{} backend ready", VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
    graphics->Ready();
    return graphics;
}

VulkanGraphics::VulkanGraphics(const NativeWindow& window, VulkanBase base, VulkanDevice device, VkPhysicalDevice physical, uint32_t family, size_t maxTextureSide) : Graphics(window), base_(std::move(base)), device_(std::move(device)), physical_(physical), family_(family), maxTextureSide_(maxTextureSide) {
    base_.GetInstance()->getPhysicalDeviceMemoryProperties(physical_, &memory_);
    device_->getDeviceQueue(device_, family_, 0, &queue_);
}

VulkanGraphics::~VulkanGraphics() {
    if (ImGui::GetCurrentContext()) {
        // Every texture goes with the device; Dear ImGui asks the next backend for them again.
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->GetTexID() == ImTextureID_Invalid) {
                continue;
            }
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
    }
    device_->deviceWaitIdle(device_);
    for (const auto& [id, texture] : textures_) {
        DestroyTexture(texture);
    }
    DestroyBuffer(vertices_);
    DestroyBuffer(indices_);
    Discard(target_);
    device_->destroyPipeline(device_, pipeline_, nullptr);
    device_->destroyRenderPass(device_, renderPass_, nullptr);
    device_->destroySampler(device_, sampler_, nullptr);
    device_->destroyDescriptorPool(device_, descriptors_, nullptr);
    device_->destroyPipelineLayout(device_, layout_, nullptr);
    device_->destroyDescriptorSetLayout(device_, setLayout_, nullptr);
    device_->destroyFence(device_, idle_, nullptr);
    device_->destroySemaphore(device_, drawn_, nullptr);
    device_->destroySemaphore(device_, acquired_, nullptr);
    device_->destroyCommandPool(device_, pool_, nullptr);
}

// Everything that does not depend on the size of the window.
Result<> VulkanGraphics::Prepare() {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = family_;
    TRY(VulkanLoader::Check("vkCreateCommandPool", device_->createCommandPool(device_, &pool, nullptr, &pool_)));
    VkCommandBufferAllocateInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool = pool_;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    TRY(VulkanLoader::Check("vkAllocateCommandBuffers", device_->allocateCommandBuffers(device_, &commands, &commands_)));
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    TRY(VulkanLoader::Check("vkCreateSemaphore", device_->createSemaphore(device_, &semaphore, nullptr, &acquired_)));
    TRY(VulkanLoader::Check("vkCreateSemaphore", device_->createSemaphore(device_, &semaphore, nullptr, &drawn_)));
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    TRY(VulkanLoader::Check("vkCreateFence", device_->createFence(device_, &fence, nullptr, &idle_)));

    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setLayout{};
    setLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayout.bindingCount = 1;
    setLayout.pBindings = &binding;
    TRY(VulkanLoader::Check("vkCreateDescriptorSetLayout", device_->createDescriptorSetLayout(device_, &setLayout, nullptr, &setLayout_)));
    const VkPushConstantRange constants{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float[4])};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &setLayout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &constants;
    TRY(VulkanLoader::Check("vkCreatePipelineLayout", device_->createPipelineLayout(device_, &layout, nullptr, &layout_)));
    const VkDescriptorPoolSize sizes{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MostTextures};
    VkDescriptorPoolCreateInfo descriptors{};
    descriptors.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptors.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptors.maxSets = MostTextures;
    descriptors.poolSizeCount = 1;
    descriptors.pPoolSizes = &sizes;
    TRY(VulkanLoader::Check("vkCreateDescriptorPool", device_->createDescriptorPool(device_, &descriptors, nullptr, &descriptors_)));

    // Every texture of Dear ImGui is sampled the same way, so one sampler serves them all.
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    TRY(VulkanLoader::Check("vkCreateSampler", device_->createSampler(device_, &sampler, nullptr, &sampler_)));

    // Colours are blended as they are, without conversion, like the other backends do.
    const VulkanInstance& instance = base_.GetInstance();
    const auto formats = TRY(Enumerate<VkSurfaceFormatKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR", [&](uint32_t* count, VkSurfaceFormatKHR* items) { return instance->getPhysicalDeviceSurfaceFormats(physical_, base_.GetSurface(), count, items); }));
    for (const VkFormat wanted : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        const auto found = std::ranges::find(formats, wanted, &VkSurfaceFormatKHR::format);
        if (found == formats.end()) {
            continue;
        }
        format_ = *found;
        break;
    }
    if (format_.format == VK_FORMAT_UNDEFINED) {
        return Err("the window offers no 8-bit RGBA format to Vulkan");
    }

    VkAttachmentDescription attachment{};
    attachment.format = format_.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    TRY(VulkanLoader::Check("vkCreateRenderPass", device_->createRenderPass(device_, &pass, nullptr, &renderPass_)));
    pipeline_ = TRY(CreatePipeline());
    return {};
}

Result<VkPipeline> VulkanGraphics::CreatePipeline() const {
    // The two shaders as glslc compiled them.
    static constexpr uint32_t VertexShader[] = {
#include "Graphics/Vulkan/Shaders/Ui.vert.spv.inc"
    };
    static constexpr uint32_t FragmentShader[] = {
#include "Graphics/Vulkan/Shaders/Ui.frag.spv.inc"
    };
    const auto module = [&](std::span<const uint32_t> code) -> Result<VkShaderModule> {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size_bytes();
        info.pCode = code.data();
        VkShaderModule made = VK_NULL_HANDLE;
        TRY(VulkanLoader::Check("vkCreateShaderModule", device_->createShaderModule(device_, &info, nullptr, &made)));
        return made;
    };
    const VkShaderModule vertex = TRY(module(VertexShader));
    const auto fragment = module(FragmentShader);
    if (!fragment) {
        device_->destroyShaderModule(device_, vertex, nullptr);
        return std::unexpected(fragment.error());
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = *fragment;
    stages[0].pName = stages[1].pName = "main";
    const VkVertexInputBindingDescription binding{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)},
    };
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attributes));
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo samples{};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Straight-alpha colours blended like this leave premultiplied ones in the window, which
    // is what the system composes.
    VkPipelineColorBlendAttachmentState blending{};
    blending.blendEnable = VK_TRUE;
    blending.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blending.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blending.colorBlendOp = VK_BLEND_OP_ADD;
    blending.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blending.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blending.alphaBlendOp = VK_BLEND_OP_ADD;
    blending.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blending;
    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(dynamics));
    dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    info.renderPass = renderPass_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult created = device_->createGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    device_->destroyShaderModule(device_, vertex, nullptr);
    device_->destroyShaderModule(device_, *fragment, nullptr);
    TRY(VulkanLoader::Check("vkCreateGraphicsPipelines", created));
    return pipeline;
}

// Makes a swapchain for the window as it is now, replacing the previous one.
Result<> VulkanGraphics::RebuildTarget() {
    TRY(VulkanLoader::Check("vkDeviceWaitIdle", device_->deviceWaitIdle(device_)));
    VkSurfaceCapabilitiesKHR capabilities{};
    TRY(VulkanLoader::Check("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", base_.GetInstance()->getPhysicalDeviceSurfaceCapabilities(physical_, base_.GetSurface(), &capabilities)));
    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0 || extent.height == 0 || extent.width == UINT32_MAX) {
        return Err("the window has no size Vulkan can draw at");
    }
    // The window must stay see-through, and it is drawn the way the display is held.
    VkCompositeAlphaFlagBitsKHR alpha{};
    for (const VkCompositeAlphaFlagBitsKHR wanted : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if (!alpha && (capabilities.supportedCompositeAlpha & wanted)) {
            alpha = wanted;
        }
    }
    if (!alpha) {
        return Err("Vulkan can only draw this window opaque");
    }
    const bool upright = capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    const uint32_t most = capabilities.maxImageCount == 0 ? UINT32_MAX : capabilities.maxImageCount;

    const VulkanTarget previous = std::exchange(target_, {});
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = base_.GetSurface();
    info.minImageCount = std::min(capabilities.minImageCount + 1, most);
    info.imageFormat = format_.format;
    info.imageColorSpace = format_.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = upright ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : capabilities.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = previous.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult created = device_->createSwapchain(device_, &info, nullptr, &swapchain);
    Discard(previous);
    TRY(VulkanLoader::Check("vkCreateSwapchainKHR", created));
    target_.swapchain = swapchain;
    target_.extent = extent;

    const auto images = TRY(Enumerate<VkImage>("vkGetSwapchainImagesKHR", [&](uint32_t* count, VkImage* items) { return device_->getSwapchainImages(device_, target_.swapchain, count, items); }));
    for (const VkImage image : images) {
        target_.views.push_back(TRY(CreateView(image, format_.format)));
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = renderPass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &target_.views.back();
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        VkFramebuffer made = VK_NULL_HANDLE;
        TRY(VulkanLoader::Check("vkCreateFramebuffer", device_->createFramebuffer(device_, &framebuffer, nullptr, &made)));
        target_.framebuffers.push_back(made);
    }
    return {};
}

void VulkanGraphics::Discard(const VulkanTarget& target) const {
    for (const VkFramebuffer framebuffer : target.framebuffers) {
        device_->destroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (const VkImageView view : target.views) {
        device_->destroyImageView(device_, view, nullptr);
    }
    device_->destroySwapchain(device_, target.swapchain, nullptr);
}

Result<VkImageView> VulkanGraphics::CreateView(VkImage image, VkFormat format) const {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = WholeImage;
    VkImageView view = VK_NULL_HANDLE;
    TRY(VulkanLoader::Check("vkCreateImageView", device_->createImageView(device_, &info, nullptr, &view)));
    return view;
}

Result<VkDeviceMemory> VulkanGraphics::Allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags flags) const {
    for (uint32_t kind = 0; kind < memory_.memoryTypeCount; kind++) {
        if ((needs.memoryTypeBits & (1u << kind)) == 0 || (memory_.memoryTypes[kind].propertyFlags & flags) != flags) {
            continue;
        }
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = needs.size;
        info.memoryTypeIndex = kind;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        TRY(VulkanLoader::Check("vkAllocateMemory", device_->allocateMemory(device_, &info, nullptr, &memory)));
        return memory;
    }
    return Err("the Vulkan device has no suitable memory");
}

// A buffer the CPU can write to, at least `size` bytes long.
Result<VulkanBuffer> VulkanGraphics::CreateBuffer(VkBufferUsageFlags usage, VkDeviceSize size) const {
    VulkanBuffer made;
    made.usage = usage;
    // Never less than 64 KB: a smaller buffer is outgrown by the next frame.
    made.capacity = std::max(std::bit_ceil(size), VkDeviceSize{1} << 16);
    const auto ready = [&]() -> Result<> {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = made.capacity;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        TRY(VulkanLoader::Check("vkCreateBuffer", device_->createBuffer(device_, &info, nullptr, &made.buffer)));
        VkMemoryRequirements needs{};
        device_->getBufferMemoryRequirements(device_, made.buffer, &needs);
        made.memory = TRY(Allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        TRY(VulkanLoader::Check("vkBindBufferMemory", device_->bindBufferMemory(device_, made.buffer, made.memory, 0)));
        void* mapped = nullptr;
        TRY(VulkanLoader::Check("vkMapMemory", device_->mapMemory(device_, made.memory, 0, VK_WHOLE_SIZE, 0, &mapped)));
        made.mapped = static_cast<uint8_t*>(mapped);
        return {};
    }();
    if (!ready) {
        DestroyBuffer(made);
        return std::unexpected(ready.error());
    }
    return made;
}

void VulkanGraphics::DestroyBuffer(const VulkanBuffer& buffer) const {
    device_->destroyBuffer(device_, buffer.buffer, nullptr);
    device_->freeMemory(device_, buffer.memory, nullptr);
}

// Makes room for a frame's mesh data, growing the buffer first when it is too small.
Result<> VulkanGraphics::Reserve(VulkanBuffer& buffer, VkDeviceSize size) const {
    if (buffer.capacity >= std::max<VkDeviceSize>(size, 1)) {
        return {};
    }
    const VulkanBuffer larger = TRY(CreateBuffer(buffer.usage, size));
    DestroyBuffer(std::exchange(buffer, larger));
    return {};
}

Result<VulkanTexture> VulkanGraphics::CreateTexture(uint32_t width, uint32_t height) const {
    VulkanTexture texture;
    const auto ready = [&]() -> Result<> {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = TextureFormat;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        TRY(VulkanLoader::Check("vkCreateImage", device_->createImage(device_, &info, nullptr, &texture.image)));
        VkMemoryRequirements needs{};
        device_->getImageMemoryRequirements(device_, texture.image, &needs);
        texture.memory = TRY(Allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        TRY(VulkanLoader::Check("vkBindImageMemory", device_->bindImageMemory(device_, texture.image, texture.memory, 0)));
        texture.view = TRY(CreateView(texture.image, TextureFormat));

        VkDescriptorSetAllocateInfo sets{};
        sets.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sets.descriptorPool = descriptors_;
        sets.descriptorSetCount = 1;
        sets.pSetLayouts = &setLayout_;
        TRY(VulkanLoader::Check("vkAllocateDescriptorSets", device_->allocateDescriptorSets(device_, &sets, &texture.set)));
        const VkDescriptorImageInfo image{sampler_, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = texture.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        device_->updateDescriptorSets(device_, 1, &write, 0, nullptr);
        return {};
    }();
    if (!ready) {
        DestroyTexture(texture);
        return std::unexpected(ready.error());
    }
    return texture;
}

void VulkanGraphics::DestroyTexture(const VulkanTexture& texture) const {
    if (texture.set) {
        device_->freeDescriptorSets(device_, descriptors_, 1, &texture.set);
    }
    device_->destroyImageView(device_, texture.view, nullptr);
    device_->destroyImage(device_, texture.image, nullptr);
    device_->freeMemory(device_, texture.memory, nullptr);
}

// Creates, updates or destroys one of the textures Dear ImGui keeps, as it asks. No frame is
// on its way through the device when this is called, so nothing here is still in use.
Result<> VulkanGraphics::UpdateTexture(ImTextureData& texture) {
    if (texture.Status == ImTextureStatus_WantCreate) {
        const VkRect2D whole{{0, 0}, {static_cast<uint32_t>(texture.Width), static_cast<uint32_t>(texture.Height)}};
        const VulkanTexture made = TRY(CreateTexture(whole.extent.width, whole.extent.height));
        if (const auto filled = Upload(texture, made, {&whole, 1}, true); !filled) {
            DestroyTexture(made);
            return filled;
        }
        textures_.emplace(nextTexture_, made);
        texture.SetTexID(static_cast<ImTextureID>(nextTexture_++));
        texture.SetStatus(ImTextureStatus_OK);
    } else if (texture.Status == ImTextureStatus_WantUpdates) {
        if (const auto found = textures_.find(texture.GetTexID()); found != textures_.end()) {
            std::vector<VkRect2D> parts;
            for (const ImTextureRect& part : texture.Updates) {
                parts.push_back({{part.x, part.y}, {part.w, part.h}});
            }
            TRY(Upload(texture, found->second, parts, false));
        }
        texture.SetStatus(ImTextureStatus_OK);
    } else if (texture.Status == ImTextureStatus_WantDestroy && texture.UnusedFrames > 0) {
        if (const auto found = textures_.find(texture.GetTexID()); found != textures_.end()) {
            DestroyTexture(found->second);
            textures_.erase(found);
        }
        texture.SetTexID(ImTextureID_Invalid);
        texture.SetStatus(ImTextureStatus_Destroyed);
    }
    return {};
}

// Copies rectangles of a texture's pixels into its image, through a buffer the device reads.
// The image holds RGBA; a texture of one channel becomes white with that channel as alpha.
Result<> VulkanGraphics::Upload(ImTextureData& source, const VulkanTexture& texture, std::span<const VkRect2D> parts, bool fresh) const {
    VkDeviceSize size = 0;
    for (const VkRect2D& part : parts) {
        size += VkDeviceSize{part.extent.width} * part.extent.height * 4;
    }
    if (size == 0) {
        return {};
    }
    const VulkanBuffer staging = TRY(CreateBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, size));

    std::vector<VkBufferImageCopy> regions;
    uint8_t* target = staging.mapped;
    for (const VkRect2D& part : parts) {
        VkBufferImageCopy region{};
        region.bufferOffset = static_cast<VkDeviceSize>(target - staging.mapped);
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {part.offset.x, part.offset.y, 0};
        region.imageExtent = {part.extent.width, part.extent.height, 1};
        regions.push_back(region);
        const size_t width = part.extent.width;
        for (uint32_t row = 0; row < part.extent.height; row++, target += width * 4) {
            const auto* pixels = static_cast<const uint8_t*>(source.GetPixelsAt(part.offset.x, part.offset.y + static_cast<int>(row)));
            if (source.Format == ImTextureFormat_RGBA32) {
                std::memcpy(target, pixels, width * 4);
            } else {
                for (size_t column = 0; column < width; column++) {
                    target[column * 4 + 0] = target[column * 4 + 1] = target[column * 4 + 2] = 255;
                    target[column * 4 + 3] = pixels[column];
                }
            }
        }
    }
    const Result<> copied = CopyToImage(staging.buffer, texture.image, regions, fresh);
    DestroyBuffer(staging);
    return copied;
}

Result<> VulkanGraphics::CopyToImage(VkBuffer source, VkImage image, std::span<const VkBufferImageCopy> regions, bool fresh) const {
    const auto barrier = [image](VkImageLayout from, VkImageLayout to, VkAccessFlags reads, VkAccessFlags writes) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = reads;
        barrier.dstAccessMask = writes;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = WholeImage;
        return barrier;
    };
    const VkImageLayout shown = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, written = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TRY(VulkanLoader::Check("vkBeginCommandBuffer", device_->beginCommandBuffer(commands_, &begin)));
    const VkImageMemoryBarrier prepare = barrier(fresh ? VK_IMAGE_LAYOUT_UNDEFINED : shown, written, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    device_->cmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &prepare);
    device_->cmdCopyBufferToImage(commands_, source, image, written, static_cast<uint32_t>(regions.size()), regions.data());
    const VkImageMemoryBarrier finish = barrier(written, shown, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    device_->cmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &finish);
    TRY(VulkanLoader::Check("vkEndCommandBuffer", device_->endCommandBuffer(commands_)));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    TRY(VulkanLoader::Check("vkQueueSubmit", device_->queueSubmit(queue_, 1, &submit, VK_NULL_HANDLE)));
    return VulkanLoader::Check("vkQueueWaitIdle", device_->queueWaitIdle(queue_));
}

Result<> VulkanGraphics::Draw(const ImDrawData& frame) {
    static_assert(sizeof(ImDrawIdx) == 2);
    size_t vertexBytes = 0, indexBytes = 0;
    for (const ImDrawList* list : frame.CmdLists) {
        vertexBytes += static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        indexBytes += static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
    }
    TRY(Reserve(vertices_, vertexBytes));
    TRY(Reserve(indices_, indexBytes));
    uint8_t* vertices = vertices_.mapped;
    uint8_t* indices = indices_.mapped;
    for (const ImDrawList* list : frame.CmdLists) {
        const size_t vertexPart = static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const size_t indexPart = static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        if (vertexPart) {
            std::memcpy(vertices, list->VtxBuffer.Data, vertexPart);
        }
        if (indexPart) {
            std::memcpy(indices, list->IdxBuffer.Data, indexPart);
        }
        vertices += vertexPart;
        indices += indexPart;
    }

    // A swapchain that is called suboptimal still hands out its image and still shows it; one
    // that is out of date does neither and is made again.
    uint32_t image = 0;
    for (;;) {
        const VkResult acquired = device_->acquireNextImage(device_, target_.swapchain, UINT64_MAX, acquired_, VK_NULL_HANDLE, &image);
        if (acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR) {
            break;
        }
        if (acquired != VK_ERROR_OUT_OF_DATE_KHR) {
            return Err("vkAcquireNextImageKHR failed: {}", VulkanLoader::Describe(acquired));
        }
        TRY(RebuildTarget());
    }

    const VkCommandBuffer commands = commands_;
    const VkExtent2D extent = target_.extent;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TRY(VulkanLoader::Check("vkBeginCommandBuffer", device_->beginCommandBuffer(commands, &begin)));
    const VkClearValue clear{};
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = renderPass_;
    pass.framebuffer = target_.framebuffers[image];
    pass.renderArea = {{0, 0}, extent};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    device_->cmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    device_->cmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    device_->cmdSetViewport(commands, 0, 1, &viewport);
    const ImVec2 origin = frame.DisplayPos, zoom = frame.FramebufferScale;
    const ImVec2 scale(2.0f * zoom.x / viewport.width, 2.0f * zoom.y / viewport.height);
    const float screen[4] = {scale.x, scale.y, -1.0f - origin.x * scale.x, -1.0f - origin.y * scale.y};
    device_->cmdPushConstants(commands, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof screen, screen);
    const VkDeviceSize start = 0;
    device_->cmdBindVertexBuffers(commands, 0, 1, &vertices_.buffer, &start);
    device_->cmdBindIndexBuffer(commands, indices_.buffer, 0, VK_INDEX_TYPE_UINT16);

    const auto pixel = [](float value, uint32_t most) { return static_cast<uint32_t>(std::clamp(value, 0.0f, static_cast<float>(most))); };
    uint32_t firstVertex = 0, firstIndex = 0;
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (const ImDrawList* list : frame.CmdLists) {
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState) {
                    command.UserCallback(list, &command);
                }
                continue;
            }
            const uint32_t left = pixel((command.ClipRect.x - origin.x) * zoom.x, extent.width), top = pixel((command.ClipRect.y - origin.y) * zoom.y, extent.height);
            const uint32_t right = pixel((command.ClipRect.z - origin.x) * zoom.x, extent.width), bottom = pixel((command.ClipRect.w - origin.y) * zoom.y, extent.height);
            const auto texture = textures_.find(command.GetTexID());
            if (texture == textures_.end() || right <= left || bottom <= top) {
                continue;
            }
            const VkRect2D scissor{{static_cast<int32_t>(left), static_cast<int32_t>(top)}, {right - left, bottom - top}};
            device_->cmdSetScissor(commands, 0, 1, &scissor);
            if (texture->second.set != bound) {
                bound = texture->second.set;
                device_->cmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &bound, 0, nullptr);
            }
            device_->cmdDrawIndexed(commands, command.ElemCount, 1, firstIndex + command.IdxOffset, static_cast<int32_t>(firstVertex + command.VtxOffset), 0);
        }
        firstVertex += static_cast<uint32_t>(list->VtxBuffer.Size);
        firstIndex += static_cast<uint32_t>(list->IdxBuffer.Size);
    }
    device_->cmdEndRenderPass(commands);
    TRY(VulkanLoader::Check("vkEndCommandBuffer", device_->endCommandBuffer(commands)));

    TRY(VulkanLoader::Check("vkResetFences", device_->resetFences(device_, 1, &idle_)));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired_;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &drawn_;
    TRY(VulkanLoader::Check("vkQueueSubmit", device_->queueSubmit(queue_, 1, &submit, idle_)));
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &drawn_;
    present.swapchainCount = 1;
    present.pSwapchains = &target_.swapchain;
    present.pImageIndices = &image;
    const VkResult presented = device_->queuePresent(queue_, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
        return RebuildTarget();
    }
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
        return Err("vkQueuePresentKHR failed: {}", VulkanLoader::Describe(presented));
    }
    return {};
}

Result<> VulkanGraphics::Rebind(const NativeWindow& window) {
    TRY(VulkanLoader::Check("vkDeviceWaitIdle", device_->deviceWaitIdle(device_)));
    if (window.Get() != base_.GetWindow()) {
        Discard(std::exchange(target_, {}));
        TRY(base_.Attach(window.Get()));
        VkBool32 presents = VK_FALSE;
        TRY(VulkanLoader::Check("vkGetPhysicalDeviceSurfaceSupportKHR", base_.GetInstance()->getPhysicalDeviceSurfaceSupport(physical_, family_, base_.GetSurface(), &presents)));
        if (!presents) {
            return Err("the Vulkan device cannot draw to the new window");
        }
    }
    return RebuildTarget();
}

Result<> VulkanGraphics::Render(const NativeWindow&, ImDrawData& frame) {
    // Once the frame before this one has left the device, its buffers can be written again
    // and its textures changed or destroyed.
    TRY(VulkanLoader::Check("vkWaitForFences", device_->waitForFences(device_, 1, &idle_, VK_TRUE, UINT64_MAX)));
    if (frame.Textures) {
        for (ImTextureData* texture : *frame.Textures) {
            if (texture->Status != ImTextureStatus_OK) {
                TRY(UpdateTexture(*texture));
            }
        }
    }
    if (frame.DisplaySize.x <= 0.0f || frame.DisplaySize.y <= 0.0f) {
        return {};
    }
    return Draw(frame);
}
