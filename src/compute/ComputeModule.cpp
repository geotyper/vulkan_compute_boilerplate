#include "vkexp/compute/ComputeModule.hpp"

#include "vkexp/core/VulkanContext.hpp"
#include "vkexp/demo/DemoState.hpp"
#include "vkexp/profiling/Profiler.hpp"

#include <array>
#include <stdexcept>

namespace vkexp {

ComputeModule::ComputeModule(DemoState& state, Profiler& profiler)
    : state_(state), metric_(profiler.registerMetric("Compute blur")) {}

void ComputeModule::onAttach(AppContext& context) {
    const VkDevice device = context.vulkan.device();

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1] = bindings[0];
    bindings[1].binding = 1;
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorLayoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    descriptorLayoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &descriptorLayoutInfo, nullptr,
                                    descriptorSetLayout_.put(device)) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create blur descriptor set layout");
    }

    const VkDescriptorSetLayout descriptorSetLayout = descriptorSetLayout_.get();
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, descriptorPool_.put(device)) !=
        VK_SUCCESS) {
        throw std::runtime_error("Unable to create blur descriptor pool");
    }
    VkDescriptorSetAllocateInfo descriptorAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptorAllocation.descriptorPool = descriptorPool_;
    descriptorAllocation.descriptorSetCount = 1;
    descriptorAllocation.pSetLayouts = &descriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &descriptorAllocation, &descriptorSet_) != VK_SUCCESS) {
        throw std::runtime_error("Unable to allocate blur descriptor set");
    }

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.size = sizeof(int);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, pipelineLayout_.put(device)) !=
        VK_SUCCESS) {
        throw std::runtime_error("Unable to create compute pipeline layout");
    }

    const auto shader = loadShaderModule(device, VKEXP_SHADER_DIR "/experiment.comp.spv");
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader.get();
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout_;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                     nullptr, pipeline_.put(device));
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Unable to create compute pipeline");
    }
    createOutput(context);
    updateDescriptors(context);
    sourceGeneration_ = state_.viewport.generation;
}

void ComputeModule::createOutput(AppContext& context) {
    const VkExtent2D extent = state_.viewport.extent;
    output_.create(context.vulkan.physicalDevice(), context.vulkan.device(),
                   ImageResourceConfig{
                       extent,
                       outputFormat,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   });

    outputLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    state_.blur.imageView = output_.view();
    state_.blur.sampler = output_.sampler();
    state_.blur.extent = extent;
    state_.blur.ready = false;
    ++state_.blur.generation;
}

void ComputeModule::destroyOutput() {
    state_.blur.imageView = VK_NULL_HANDLE;
    state_.blur.sampler = VK_NULL_HANDLE;
    state_.blur.ready = false;
    output_.reset();
    outputLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void ComputeModule::updateDescriptors(AppContext& context) {
    const VkDescriptorImageInfo sourceInfo{VK_NULL_HANDLE, state_.viewport.imageView,
                                           VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo outputInfo{VK_NULL_HANDLE, output_.view(), VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &sourceInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &outputInfo;
    vkUpdateDescriptorSets(context.vulkan.device(), static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void ComputeModule::onUpdate(AppContext& context, const FrameInfo&) {
    if (sourceGeneration_ == state_.viewport.generation) {
        return;
    }
    context.vulkan.waitIdle();
    destroyOutput();
    createOutput(context);
    updateDescriptors(context);
    sourceGeneration_ = state_.viewport.generation;
}

void ComputeModule::onRender(AppContext& context, const FrameInfo&) {
    if (!state_.preset.computeEnabled) {
        state_.blur.requested = false;
        return;
    }
    if (!state_.blur.requested) {
        return;
    }

    auto cpuScope = context.profiler.cpu().scope(metric_);
    auto gpuScope = context.profiler.gpu().scope(context.vulkan.commandBuffer(), metric_);
    const VkCommandBuffer commands = context.vulkan.commandBuffer();
    std::array<VkImageMemoryBarrier2, 2> toCompute{};
    for (auto& barrier : toCompute) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    toCompute[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toCompute[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    toCompute[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    toCompute[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toCompute[0].image = state_.viewport.image;

    toCompute[1].srcStageMask = outputLayout_ == VK_IMAGE_LAYOUT_UNDEFINED
                                    ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                    : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toCompute[1].srcAccessMask =
        outputLayout_ == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toCompute[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toCompute[1].oldLayout = outputLayout_;
    toCompute[1].image = output_.image();

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(toCompute.size());
    dependency.pImageMemoryBarriers = toCompute.data();
    vkCmdPipelineBarrier2(commands, &dependency);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1,
                            &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(commands, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int),
                       &state_.blur.radius);
    vkCmdDispatch(commands, (state_.blur.extent.width + 7U) / 8U,
                  (state_.blur.extent.height + 7U) / 8U, 1);

    std::array<VkImageMemoryBarrier2, 2> toSampled{};
    for (auto& barrier : toSampled) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
    }
    toSampled[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    toSampled[0].image = state_.viewport.image;
    toSampled[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toSampled[1].image = output_.image();
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(toSampled.size());
    dependency.pImageMemoryBarriers = toSampled.data();
    vkCmdPipelineBarrier2(commands, &dependency);

    outputLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    state_.blur.requested = false;
    state_.blur.ready = true;
}

void ComputeModule::onDetach(AppContext&) {
    destroyOutput();
    pipeline_.reset();
    pipelineLayout_.reset();
    descriptorPool_.reset();
    descriptorSetLayout_.reset();
    descriptorSet_ = VK_NULL_HANDLE;
}

} // namespace vkexp
