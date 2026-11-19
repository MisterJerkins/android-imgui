#pragma once

#include "Graphics/Graphics.h"
#include "Graphics/Vulkan/VulkanLoader.h"

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

struct ANativeWindow;
struct ImTextureData;

// The instance and the surface on the overlay's window; outlives everything made from them.
class VulkanBase {
public:
    static Result<VulkanBase> Create(ANativeWindow* window);

    VulkanBase(VulkanBase&& other) noexcept;
    VulkanBase& operator=(VulkanBase&&) = delete;
    ~VulkanBase();

    // Gives up the surface and makes one on `window`.
    Result<> Attach(ANativeWindow* window);

    const VulkanInstance& GetInstance() const {
        return instance_;
    }
    VkSurfaceKHR GetSurface() const {
        return surface_;
    }
    ANativeWindow* GetWindow() const {
        return window_;
    }

private:
    explicit VulkanBase(VulkanInstance instance) : instance_(std::move(instance)) {
    }

    VulkanInstance instance_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    ANativeWindow* window_ = nullptr;
};

// What a frame is drawn into: the swapchain and a framebuffer for each of its images.
struct VulkanTarget {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
};

// Memory the CPU writes mesh data into each frame; it grows and never shrinks.
struct VulkanBuffer {
    VkBufferUsageFlags usage = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
    uint8_t* mapped = nullptr;
};

struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

// Draws with Vulkan 1.0. The handles never leave this backend; whatever is still alive at the
// end is destroyed by the destructor, each handle exactly once and in the order Vulkan asks for.
class VulkanGraphics final : public Graphics {
public:
    // The graphics on `window`, ready for frames.
    static Result<std::unique_ptr<Graphics>> Create(const NativeWindow& window);
    ~VulkanGraphics() override;

    const char* GetName() const override {
        return "Vulkan";
    }

private:
    struct Choice;

    VulkanGraphics(const NativeWindow& window, VulkanBase base, VulkanDevice device, VkPhysicalDevice physical, uint32_t family, size_t maxTextureSide);

    size_t GetMaxTextureSide() const override {
        return maxTextureSide_;
    }
    Result<> Rebind(const NativeWindow& window) override;
    Result<> Render(const NativeWindow& window, ImDrawData& frame) override;

    template <class T, class Call>
    static Result<std::vector<T>> Enumerate(const char* what, Call call);
    static Result<Choice> ChooseDevice(const VulkanBase& base);
    Result<> Prepare();
    Result<VkPipeline> CreatePipeline() const;
    Result<> RebuildTarget();
    void Discard(const VulkanTarget& target) const;
    Result<VkImageView> CreateView(VkImage image, VkFormat format) const;
    Result<VkDeviceMemory> Allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags flags) const;
    Result<VulkanBuffer> CreateBuffer(VkBufferUsageFlags usage, VkDeviceSize size) const;
    void DestroyBuffer(const VulkanBuffer& buffer) const;
    Result<> Reserve(VulkanBuffer& buffer, VkDeviceSize size) const;
    Result<VulkanTexture> CreateTexture(uint32_t width, uint32_t height) const;
    void DestroyTexture(const VulkanTexture& texture) const;
    Result<> UpdateTexture(ImTextureData& texture);
    Result<> Upload(ImTextureData& source, const VulkanTexture& texture, std::span<const VkRect2D> parts, bool fresh) const;
    Result<> CopyToImage(VkBuffer source, VkImage image, std::span<const VkBufferImageCopy> regions, bool fresh) const;
    Result<> Draw(const ImDrawData& frame);

    // Members go away last to first: the device after everything the destructor releases, the
    // instance and its surface after the device.
    VulkanBase base_;
    VulkanDevice device_;
    VkPhysicalDevice physical_;
    VkPhysicalDeviceMemoryProperties memory_{};
    uint32_t family_;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format_{};
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptors_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    VkSemaphore drawn_ = VK_NULL_HANDLE;
    VkFence idle_ = VK_NULL_HANDLE;
    VulkanTarget target_;
    VulkanBuffer vertices_{.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VulkanBuffer indices_{.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    // By the identifier Dear ImGui is given for the texture.
    std::unordered_map<uint64_t, VulkanTexture> textures_;
    uint64_t nextTexture_ = 1;
    size_t maxTextureSide_;
};
