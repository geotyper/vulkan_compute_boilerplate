#include "vkexp/compute/ComputeResources.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

class VulkanUnavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct InstanceOwner {
    VkInstance value{};
    ~InstanceOwner() {
        if (value != VK_NULL_HANDLE) {
            vkDestroyInstance(value, nullptr);
        }
    }
};

struct DeviceOwner {
    VkDevice value{};
    ~DeviceOwner() {
        if (value != VK_NULL_HANDLE) {
            vkDestroyDevice(value, nullptr);
        }
    }
};

struct GridSize {
    std::uint32_t width;
    std::uint32_t height;
};

int run() {
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "vkexp compute smoke";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    InstanceOwner instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance.value) != VK_SUCCESS) {
        throw VulkanUnavailable("Vulkan 1.3 instance is unavailable");
    }

    std::uint32_t physicalDeviceCount{};
    if (vkEnumeratePhysicalDevices(instance.value, &physicalDeviceCount, nullptr) != VK_SUCCESS ||
        physicalDeviceCount == 0) {
        throw VulkanUnavailable("No Vulkan physical device is available");
    }
    std::vector<VkPhysicalDevice> devices(physicalDeviceCount);
    vkEnumeratePhysicalDevices(instance.value, &physicalDeviceCount, devices.data());

    VkPhysicalDevice physicalDevice{};
    std::uint32_t queueFamily{};
    for (const VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        VkPhysicalDeviceVulkan13Features features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &features;
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (properties.apiVersion < VK_API_VERSION_1_3 || features.synchronization2 != VK_TRUE) {
            continue;
        }

        std::uint32_t familyCount{};
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (std::uint32_t index = 0; index < familyCount; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                physicalDevice = candidate;
                queueFamily = index;
                break;
            }
        }
        if (physicalDevice != VK_NULL_HANDLE) {
            break;
        }
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        throw VulkanUnavailable("No Vulkan 1.3 compute queue with synchronization2 is available");
    }

    constexpr float queuePriority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;
    VkPhysicalDeviceVulkan13Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    DeviceOwner device;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device.value) != VK_SUCCESS) {
        throw VulkanUnavailable("Unable to create the Vulkan compute device");
    }
    VkQueue queue{};
    vkGetDeviceQueue(device.value, queueFamily, 0, &queue);

    constexpr GridSize grid{16, 16};
    constexpr std::size_t cellCount = grid.width * grid.height;
    constexpr VkDeviceSize byteSize = cellCount * sizeof(std::uint32_t);
    std::array<std::uint32_t, cellCount> initial{};
    const std::size_t center = (grid.height / 2) * grid.width + grid.width / 2;
    initial[center - 1] = 1;
    initial[center] = 1;
    initial[center + 1] = 1;

    vkexp::PingPongBuffer state;
    state.create(physicalDevice, device.value,
                 {byteSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT});
    vkexp::ImmediateContext immediate{physicalDevice, device.value, queueFamily, queue};
    immediate.uploadBuffer(state.read(), initial.data(), byteSize);

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    vkexp::UniqueDescriptorSetLayout setLayout;
    if (vkCreateDescriptorSetLayout(device.value, &layoutInfo, nullptr,
                                    setLayout.put(device.value)) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create smoke descriptor set layout");
    }

    vkexp::DescriptorAllocator descriptors{
        device.value,
        {1, {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(bindings.size())}}}};
    const VkDescriptorSet descriptorSet = descriptors.allocate(setLayout.get());
    vkexp::DescriptorSetWriter{}
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, state.read().buffer())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, state.write().buffer())
        .update(device.value, descriptorSet);
    const vkexp::ComputePipeline pipeline =
        vkexp::ComputePipelineBuilder{device.value}
            .shader(VKEXP_SHADER_DIR "/game_of_life.comp.spv")
            .addDescriptorSetLayout(setLayout.get())
            .addPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(GridSize))
            .build();

    immediate.execute([&](const VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline());
        const VkPipelineLayout pipelineLayout = pipeline.layout();
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                &descriptorSet, 0, nullptr);
        vkCmdPushConstants(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(GridSize), &grid);
        const vkexp::DispatchSize groups =
            vkexp::dispatchSize({grid.width, grid.height, 1}, {8, 8, 1});
        vkCmdDispatch(commands, groups.x, groups.y, groups.z);
    });
    state.swap();

    std::array<std::uint32_t, cellCount> result{};
    immediate.readbackBuffer(state.read(), result.data(), byteSize);
    std::array<std::uint32_t, cellCount> expected{};
    expected[center - grid.width] = 1;
    expected[center] = 1;
    expected[center + grid.width] = 1;
    if (result != expected) {
        throw std::runtime_error("Game of Life GPU result did not match the expected blinker");
    }
    std::cout << "Headless compute smoke test passed\n";
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const VulkanUnavailable& error) {
        std::cout << "SKIPPED: " << error.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
