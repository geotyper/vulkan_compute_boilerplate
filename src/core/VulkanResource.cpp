#include "vkexp/core/VulkanResource.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vkexp {
namespace {

std::uint32_t findMemoryType(const VkPhysicalDevice physicalDevice, const std::uint32_t typeFilter,
                             const VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
        const bool supported = (typeFilter & (1U << index)) != 0U;
        const bool matches =
            (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties;
        if (supported && matches) {
            return index;
        }
    }
    throw std::runtime_error("Unable to find a suitable Vulkan memory type");
}

} // namespace

void ImageResource::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                           const ImageResourceConfig& config) {
    reset();

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = config.format;
    imageInfo.extent = {config.extent.width, config.extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = config.usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image{};
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create Vulkan image");
    }
    image_.reset(device, image);

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image_.get(), &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        findMemoryType(physicalDevice, requirements.memoryTypeBits, config.memoryProperties);
    VkDeviceMemory memory{};
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Unable to allocate Vulkan image memory");
    }
    memory_.reset(device, memory);
    if (vkBindImageMemory(device, image_.get(), memory_.get(), 0) != VK_SUCCESS) {
        throw std::runtime_error("Unable to bind Vulkan image memory");
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image_.get();
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = config.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view{};
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create Vulkan image view");
    }
    view_.reset(device, view);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = config.filter;
    samplerInfo.minFilter = config.filter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0F;
    VkSampler sampler{};
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create Vulkan sampler");
    }
    sampler_.reset(device, sampler);
    extent_ = config.extent;
}

void ImageResource::reset() {
    sampler_.reset();
    view_.reset();
    image_.reset();
    memory_.reset();
    extent_ = {};
}

UniqueShaderModule loadShaderModule(const VkDevice device, const std::string_view path) {
    std::ifstream file(std::string{path}, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Unable to open shader: " + std::string{path});
    }
    const auto size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        throw std::runtime_error("Invalid SPIR-V file: " + std::string{path});
    }
    std::vector<std::byte> code(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
    VkShaderModule shader{};
    if (vkCreateShaderModule(device, &info, nullptr, &shader) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create shader module: " + std::string{path});
    }
    return UniqueShaderModule{device, shader};
}

} // namespace vkexp
