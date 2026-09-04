#pragma once

#include "vkexp/core/VulkanResource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vkexp {

struct DispatchSize {
    std::uint32_t x{};
    std::uint32_t y{1};
    std::uint32_t z{1};
};

[[nodiscard]] std::uint32_t divideRoundUp(std::uint32_t value, std::uint32_t divisor);
[[nodiscard]] DispatchSize dispatchSize(VkExtent3D problemSize, VkExtent3D localSize);

void cmdBufferBarrier(VkCommandBuffer commands, VkBuffer buffer, VkPipelineStageFlags2 sourceStage,
                      VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                      VkAccessFlags2 destinationAccess, VkDeviceSize offset = 0,
                      VkDeviceSize size = VK_WHOLE_SIZE);
void cmdImageBarrier(VkCommandBuffer commands, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                     VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                     VkAccessFlags2 destinationAccess,
                     VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
void cmdComputeWriteToComputeRead(VkCommandBuffer commands, VkBuffer buffer,
                                  VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

struct DescriptorAllocatorConfig {
    std::uint32_t maxSets{16};
    std::vector<VkDescriptorPoolSize> poolSizes;
};

class DescriptorAllocator {
public:
    DescriptorAllocator() = default;
    DescriptorAllocator(VkDevice device, const DescriptorAllocatorConfig& config);

    DescriptorAllocator(const DescriptorAllocator&) = delete;
    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;
    DescriptorAllocator(DescriptorAllocator&&) noexcept = default;
    DescriptorAllocator& operator=(DescriptorAllocator&&) noexcept = default;

    void create(VkDevice device, const DescriptorAllocatorConfig& config);
    void reset();
    void resetSets() const;
    [[nodiscard]] VkDescriptorSet allocate(VkDescriptorSetLayout layout) const;
    [[nodiscard]] VkDescriptorPool pool() const { return pool_.get(); }

private:
    VkDevice device_{};
    UniqueDescriptorPool pool_;
};

class DescriptorSetWriter {
public:
    DescriptorSetWriter& writeBuffer(std::uint32_t binding, VkDescriptorType type, VkBuffer buffer,
                                     VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE,
                                     std::uint32_t arrayElement = 0);
    DescriptorSetWriter& writeImage(std::uint32_t binding, VkDescriptorType type, VkImageView view,
                                    VkImageLayout layout, VkSampler sampler = VK_NULL_HANDLE,
                                    std::uint32_t arrayElement = 0);
    void update(VkDevice device, VkDescriptorSet set) const;
    void clear() { writes_.clear(); }

private:
    struct PendingWrite {
        std::uint32_t binding{};
        std::uint32_t arrayElement{};
        VkDescriptorType type{};
        bool image{};
        VkDescriptorBufferInfo bufferInfo{};
        VkDescriptorImageInfo imageInfo{};
    };
    std::vector<PendingWrite> writes_;
};

class ComputePipeline {
public:
    ComputePipeline() = default;

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&&) noexcept = default;
    ComputePipeline& operator=(ComputePipeline&&) noexcept = default;

    [[nodiscard]] VkPipeline pipeline() const { return pipeline_.get(); }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_.get(); }
    [[nodiscard]] explicit operator bool() const { return static_cast<bool>(pipeline_); }

private:
    friend class ComputePipelineBuilder;
    UniquePipelineLayout layout_;
    UniquePipeline pipeline_;
};

class ComputePipelineBuilder {
public:
    explicit ComputePipelineBuilder(VkDevice device) : device_(device) {}

    ComputePipelineBuilder& shader(std::string path, std::string entryPoint = "main");
    ComputePipelineBuilder& addDescriptorSetLayout(VkDescriptorSetLayout layout);
    ComputePipelineBuilder& addPushConstantRange(VkShaderStageFlags stages, std::uint32_t size,
                                                 std::uint32_t offset = 0);
    [[nodiscard]] ComputePipeline build() const;

private:
    VkDevice device_{};
    std::string shaderPath_;
    std::string entryPoint_{"main"};
    std::vector<VkDescriptorSetLayout> setLayouts_;
    std::vector<VkPushConstantRange> pushConstants_;
};

class ImmediateContext {
public:
    ImmediateContext() = default;
    ImmediateContext(VkPhysicalDevice physicalDevice, VkDevice device, std::uint32_t queueFamily,
                     VkQueue queue);

    ImmediateContext(const ImmediateContext&) = delete;
    ImmediateContext& operator=(const ImmediateContext&) = delete;
    ImmediateContext(ImmediateContext&&) noexcept = default;
    ImmediateContext& operator=(ImmediateContext&&) noexcept = default;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, std::uint32_t queueFamily,
                VkQueue queue);
    void reset();
    void execute(const std::function<void(VkCommandBuffer)>& record) const;
    void uploadBuffer(BufferResource& destination, const void* data, VkDeviceSize size,
                      VkDeviceSize destinationOffset = 0) const;
    void readbackBuffer(const BufferResource& source, void* data, VkDeviceSize size,
                        VkDeviceSize sourceOffset = 0) const;

private:
    VkPhysicalDevice physicalDevice_{};
    VkDevice device_{};
    VkQueue queue_{};
    UniqueCommandPool commandPool_;
};

class PingPongBuffer {
public:
    void create(VkPhysicalDevice physicalDevice, VkDevice device,
                const BufferResourceConfig& config);
    void reset();
    void swap() { readIndex_ ^= 1U; }

    [[nodiscard]] BufferResource& read() { return resources_[readIndex_]; }
    [[nodiscard]] const BufferResource& read() const { return resources_[readIndex_]; }
    [[nodiscard]] BufferResource& write() { return resources_[readIndex_ ^ 1U]; }
    [[nodiscard]] const BufferResource& write() const { return resources_[readIndex_ ^ 1U]; }
    [[nodiscard]] std::uint32_t readIndex() const { return readIndex_; }
    [[nodiscard]] std::uint32_t writeIndex() const { return readIndex_ ^ 1U; }

private:
    std::array<BufferResource, 2> resources_;
    std::uint32_t readIndex_{};
};

class PingPongImage {
public:
    void create(VkPhysicalDevice physicalDevice, VkDevice device,
                const ImageResourceConfig& config);
    void reset();
    void swap() { readIndex_ ^= 1U; }

    [[nodiscard]] ImageResource& read() { return resources_[readIndex_]; }
    [[nodiscard]] const ImageResource& read() const { return resources_[readIndex_]; }
    [[nodiscard]] ImageResource& write() { return resources_[readIndex_ ^ 1U]; }
    [[nodiscard]] const ImageResource& write() const { return resources_[readIndex_ ^ 1U]; }
    [[nodiscard]] std::uint32_t readIndex() const { return readIndex_; }
    [[nodiscard]] std::uint32_t writeIndex() const { return readIndex_ ^ 1U; }

private:
    std::array<ImageResource, 2> resources_;
    std::uint32_t readIndex_{};
};

} // namespace vkexp
