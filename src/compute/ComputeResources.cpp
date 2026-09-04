#include "vkexp/compute/ComputeResources.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace vkexp {

std::uint32_t divideRoundUp(const std::uint32_t value, const std::uint32_t divisor) {
    if (divisor == 0) {
        throw std::invalid_argument("Dispatch divisor cannot be zero");
    }
    return value / divisor + static_cast<std::uint32_t>(value % divisor != 0);
}

DispatchSize dispatchSize(const VkExtent3D problemSize, const VkExtent3D localSize) {
    if (localSize.width == 0 || localSize.height == 0 || localSize.depth == 0) {
        throw std::invalid_argument("Compute local size cannot contain zero");
    }
    return {
        divideRoundUp(problemSize.width, localSize.width),
        divideRoundUp(problemSize.height, localSize.height),
        divideRoundUp(problemSize.depth, localSize.depth),
    };
}

void validateComputeLimits(const VkPhysicalDeviceLimits& limits, const DispatchSize groups,
                           const VkExtent3D localSize, const std::uint32_t pushConstantBytes,
                           const std::span<const VkDeviceSize> storageBufferRanges) {
    const std::array groupCounts{groups.x, groups.y, groups.z};
    const std::array localSizes{localSize.width, localSize.height, localSize.depth};
    for (std::size_t axis = 0; axis < groupCounts.size(); ++axis) {
        if (groupCounts[axis] > limits.maxComputeWorkGroupCount[axis]) {
            throw std::out_of_range("Compute dispatch group count exceeds the device limit");
        }
        if (localSizes[axis] == 0 || localSizes[axis] > limits.maxComputeWorkGroupSize[axis]) {
            throw std::out_of_range("Compute local size exceeds the device limit");
        }
    }

    const std::uint64_t invocations =
        static_cast<std::uint64_t>(localSize.width) * localSize.height * localSize.depth;
    if (invocations > limits.maxComputeWorkGroupInvocations) {
        throw std::out_of_range("Compute local invocation count exceeds the device limit");
    }
    if (pushConstantBytes > limits.maxPushConstantsSize) {
        throw std::out_of_range("Compute push constants exceed the device limit");
    }
    for (const VkDeviceSize range : storageBufferRanges) {
        if (range == 0 || range == VK_WHOLE_SIZE || range > limits.maxStorageBufferRange) {
            throw std::out_of_range("Storage buffer descriptor range exceeds the device limit");
        }
    }
}

DispatchSize checkedDispatchSize(const VkPhysicalDevice physicalDevice,
                                 const ComputeDispatchConfig& config) {
    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::invalid_argument("Checked dispatch requires a physical device");
    }
    const DispatchSize groups = dispatchSize(config.problemSize, config.localSize);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    validateComputeLimits(properties.limits, groups, config.localSize, config.pushConstantBytes,
                          config.storageBufferRanges);
    return groups;
}

void cmdBufferBarrier(const VkCommandBuffer commands, const VkBuffer buffer,
                      const VkPipelineStageFlags2 sourceStage, const VkAccessFlags2 sourceAccess,
                      const VkPipelineStageFlags2 destinationStage,
                      const VkAccessFlags2 destinationAccess, const VkDeviceSize offset,
                      const VkDeviceSize size) {
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = sourceStage;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstStageMask = destinationStage;
    barrier.dstAccessMask = destinationAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = offset;
    barrier.size = size;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

void cmdImageBarrier(const VkCommandBuffer commands, const VkImage image,
                     const VkImageLayout oldLayout, const VkImageLayout newLayout,
                     const VkPipelineStageFlags2 sourceStage, const VkAccessFlags2 sourceAccess,
                     const VkPipelineStageFlags2 destinationStage,
                     const VkAccessFlags2 destinationAccess, const VkImageSubresourceRange range) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = sourceStage;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstStageMask = destinationStage;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

void cmdComputeWriteToComputeRead(const VkCommandBuffer commands, const VkBuffer buffer,
                                  const VkDeviceSize offset, const VkDeviceSize size) {
    cmdBufferBarrier(commands, buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     offset, size);
}

DescriptorAllocator::DescriptorAllocator(const VkDevice device,
                                         const DescriptorAllocatorConfig& config) {
    create(device, config);
}

void DescriptorAllocator::create(const VkDevice device, const DescriptorAllocatorConfig& config) {
    reset();
    if (device == VK_NULL_HANDLE || config.maxSets == 0 || config.poolSizes.empty()) {
        throw std::invalid_argument("Invalid descriptor allocator configuration");
    }
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = config.maxSets;
    info.poolSizeCount = static_cast<std::uint32_t>(config.poolSizes.size());
    info.pPoolSizes = config.poolSizes.data();
    if (vkCreateDescriptorPool(device, &info, nullptr, pool_.put(device)) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create descriptor pool");
    }
    device_ = device;
}

void DescriptorAllocator::reset() {
    pool_.reset();
    device_ = VK_NULL_HANDLE;
}

void DescriptorAllocator::resetSets() const {
    if (!pool_ || vkResetDescriptorPool(device_, pool_.get(), 0) != VK_SUCCESS) {
        throw std::runtime_error("Unable to reset descriptor pool");
    }
}

VkDescriptorSet DescriptorAllocator::allocate(const VkDescriptorSetLayout layout) const {
    if (!pool_ || layout == VK_NULL_HANDLE) {
        throw std::logic_error("Descriptor allocator is not initialized");
    }
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pool_.get();
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set{};
    if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS) {
        throw std::runtime_error("Unable to allocate descriptor set");
    }
    return set;
}

DescriptorSetWriter&
DescriptorSetWriter::writeBuffer(const std::uint32_t binding, const VkDescriptorType type,
                                 const VkBuffer buffer, const VkDeviceSize offset,
                                 const VkDeviceSize range, const std::uint32_t arrayElement) {
    if (buffer == VK_NULL_HANDLE) {
        throw std::invalid_argument("Cannot write a null buffer descriptor");
    }
    PendingWrite write{};
    write.binding = binding;
    write.arrayElement = arrayElement;
    write.type = type;
    write.bufferInfo = {buffer, offset, range};
    writes_.push_back(write);
    return *this;
}

DescriptorSetWriter&
DescriptorSetWriter::writeImage(const std::uint32_t binding, const VkDescriptorType type,
                                const VkImageView view, const VkImageLayout layout,
                                const VkSampler sampler, const std::uint32_t arrayElement) {
    if (view == VK_NULL_HANDLE) {
        throw std::invalid_argument("Cannot write a null image descriptor");
    }
    PendingWrite write{};
    write.binding = binding;
    write.arrayElement = arrayElement;
    write.type = type;
    write.image = true;
    write.imageInfo = {sampler, view, layout};
    writes_.push_back(write);
    return *this;
}

void DescriptorSetWriter::update(const VkDevice device, const VkDescriptorSet set) const {
    if (device == VK_NULL_HANDLE || set == VK_NULL_HANDLE) {
        throw std::invalid_argument("Descriptor update requires a device and set");
    }
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(writes_.size());
    for (const PendingWrite& pending : writes_) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = pending.binding;
        write.dstArrayElement = pending.arrayElement;
        write.descriptorCount = 1;
        write.descriptorType = pending.type;
        if (pending.image) {
            write.pImageInfo = &pending.imageInfo;
        } else {
            write.pBufferInfo = &pending.bufferInfo;
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
}

ComputePipelineBuilder& ComputePipelineBuilder::shader(std::string path, std::string entryPoint) {
    shaderPath_ = std::move(path);
    entryPoint_ = std::move(entryPoint);
    return *this;
}

ComputePipelineBuilder&
ComputePipelineBuilder::addDescriptorSetLayout(const VkDescriptorSetLayout layout) {
    if (layout == VK_NULL_HANDLE) {
        throw std::invalid_argument("Cannot add a null descriptor set layout");
    }
    setLayouts_.push_back(layout);
    return *this;
}

ComputePipelineBuilder&
ComputePipelineBuilder::addPushConstantRange(const VkShaderStageFlags stages,
                                             const std::uint32_t size, const std::uint32_t offset) {
    if (stages == 0 || size == 0) {
        throw std::invalid_argument("Invalid push constant range");
    }
    pushConstants_.push_back({stages, offset, size});
    return *this;
}

ComputePipeline ComputePipelineBuilder::build() const {
    if (physicalDevice_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE || shaderPath_.empty() ||
        entryPoint_.empty()) {
        throw std::logic_error("Compute pipeline builder is incomplete");
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    for (const VkPushConstantRange& range : pushConstants_) {
        if (range.offset % 4 != 0 || range.size % 4 != 0 ||
            range.offset > properties.limits.maxPushConstantsSize ||
            range.size > properties.limits.maxPushConstantsSize - range.offset) {
            throw std::out_of_range("Compute push constant range exceeds the device limit");
        }
    }
    ComputePipeline result;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts_.size());
    layoutInfo.pSetLayouts = setLayouts_.data();
    layoutInfo.pushConstantRangeCount = static_cast<std::uint32_t>(pushConstants_.size());
    layoutInfo.pPushConstantRanges = pushConstants_.data();
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, result.layout_.put(device_)) !=
        VK_SUCCESS) {
        throw std::runtime_error("Unable to create compute pipeline layout");
    }

    const UniqueShaderModule module = loadShaderModule(device_, shaderPath_);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module.get();
    stage.pName = entryPoint_.c_str();
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stage;
    pipelineInfo.layout = result.layout_.get();
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                 result.pipeline_.put(device_)) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create compute pipeline");
    }
    return result;
}

ImmediateContext::ImmediateContext(const VkPhysicalDevice physicalDevice, const VkDevice device,
                                   const std::uint32_t queueFamily, const VkQueue queue) {
    create(physicalDevice, device, queueFamily, queue);
}

void ImmediateContext::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                              const std::uint32_t queueFamily, const VkQueue queue) {
    reset();
    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        throw std::invalid_argument("Invalid immediate context configuration");
    }
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device, &info, nullptr, commandPool_.put(device)) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create staging command pool");
    }
    physicalDevice_ = physicalDevice;
    device_ = device;
    queue_ = queue;
}

void ImmediateContext::reset() {
    commandPool_.reset();
    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

void ImmediateContext::execute(const std::function<void(VkCommandBuffer)>& record) const {
    if (!commandPool_ || !record) {
        throw std::logic_error("Immediate context is not initialized");
    }
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = commandPool_.get();
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commands{};
    if (vkAllocateCommandBuffers(device_, &allocation, &commands) != VK_SUCCESS) {
        throw std::runtime_error("Unable to allocate immediate command buffer");
    }
    try {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
            throw std::runtime_error("Unable to begin immediate command buffer");
        }
        record(commands);
        if (vkEndCommandBuffer(commands) != VK_SUCCESS) {
            throw std::runtime_error("Unable to end immediate command buffer");
        }
        UniqueFence fence;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device_, &fenceInfo, nullptr, fence.put(device_)) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create immediate submission fence");
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (vkQueueSubmit(queue_, 1, &submit, fence.get()) != VK_SUCCESS) {
            throw std::runtime_error("Unable to submit immediate command buffer");
        }
        const VkFence fenceHandle = fence.get();
        if (vkWaitForFences(device_, 1, &fenceHandle, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            throw std::runtime_error("Unable to wait for immediate command buffer");
        }
    } catch (...) {
        vkFreeCommandBuffers(device_, commandPool_.get(), 1, &commands);
        throw;
    }
    vkFreeCommandBuffers(device_, commandPool_.get(), 1, &commands);
}

void ImmediateContext::uploadBuffer(BufferResource& destination, const void* data,
                                    const VkDeviceSize size,
                                    const VkDeviceSize destinationOffset) const {
    if ((destination.usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0 ||
        destinationOffset > destination.size() || size > destination.size() - destinationOffset) {
        throw std::invalid_argument("Invalid upload destination buffer or range");
    }
    BufferResource staging;
    staging.create(physicalDevice_, device_,
                   {size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT});
    staging.write(data, size);
    execute([&](const VkCommandBuffer commands) {
        const VkBufferCopy copy{0, destinationOffset, size};
        vkCmdCopyBuffer(commands, staging.buffer(), destination.buffer(), 1, &copy);
        cmdBufferBarrier(commands, destination.buffer(), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                         destinationOffset, size);
    });
}

void ImmediateContext::readbackBuffer(const BufferResource& source, void* data,
                                      const VkDeviceSize size,
                                      const VkDeviceSize sourceOffset) const {
    if ((source.usage() & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) == 0 || sourceOffset > source.size() ||
        size > source.size() - sourceOffset) {
        throw std::invalid_argument("Invalid readback source buffer or range");
    }
    BufferResource staging;
    staging.create(physicalDevice_, device_,
                   {size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT});
    execute([&](const VkCommandBuffer commands) {
        cmdBufferBarrier(commands, source.buffer(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, sourceOffset, size);
        const VkBufferCopy copy{sourceOffset, 0, size};
        vkCmdCopyBuffer(commands, source.buffer(), staging.buffer(), 1, &copy);
    });
    staging.read(data, size);
}

void PingPongBuffer::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                            const BufferResourceConfig& config) {
    reset();
    resources_[0].create(physicalDevice, device, config);
    resources_[1].create(physicalDevice, device, config);
}

void PingPongBuffer::reset() {
    resources_[0].reset();
    resources_[1].reset();
    readIndex_ = 0;
}

void PingPongImage::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                           const ImageResourceConfig& config) {
    reset();
    resources_[0].create(physicalDevice, device, config);
    resources_[1].create(physicalDevice, device, config);
}

void PingPongImage::reset() {
    resources_[0].reset();
    resources_[1].reset();
    readIndex_ = 0;
}

} // namespace vkexp
