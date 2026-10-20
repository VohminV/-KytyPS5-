#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/low_vram_downscale.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace Libs::Graphics {

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}

	LowVramDownscale::Init(allocator);
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: size=%" PRIu64 " usage=%" PRIu64 ", budget=%" PRIu64
		     ", allocation=%" PRIu64 ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(properties.memoryHeaps[i].size),
		     static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}

	std::fprintf(
	    stderr,
	    "[VMA] memory budget (heap count=%u):\n",
	    properties.memoryHeapCount);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		std::fprintf(
		    stderr,
		    "[VMA]   heap %u: size=%llu usage=%llu budget=%llu allocation=%llu blocks=%llu\n",
		    i,
		    static_cast<unsigned long long>(properties.memoryHeaps[i].size),
		    static_cast<unsigned long long>(budgets[i].usage),
		    static_cast<unsigned long long>(budgets[i].budget),
		    static_cast<unsigned long long>(budgets[i].statistics.allocationBytes),
		    static_cast<unsigned long long>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint32_t GraphicContext::GetHeapUsage(std::span<HeapUsage, VK_MAX_MEMORY_HEAPS> heaps) const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const auto count = physical_device_memory_properties.memoryHeapCount;
	for (uint32_t heap = 0; heap < count; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		heaps[heap] = {.usage        = budgets[heap].usage,
		               .budget       = budgets[heap].budget,
		               .allocations  = budgets[heap].statistics.allocationCount,
		               .device_local = static_cast<bool>(properties.flags &
		                                                 vk::MemoryHeapFlagBits::eDeviceLocal)};
	}
	return count;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VmaAllocationInfo allocation_info {};
	VkImage           native_image = VK_NULL_HANDLE;

	const auto* original_create_info =
	    reinterpret_cast<const VkImageCreateInfo*>(&image_info);
	VkImageCreateInfo create_info = *original_create_info;
	LowVramDownscale::Apply(create_info);

	const auto try_create = [&](VkImageCreateInfo& info) {
		image.allocation = nullptr;
		native_image     = VK_NULL_HANDLE;
		return vmaCreateImage(allocator, &info, &alloc_info, &native_image, &image.allocation,
		                      &allocation_info);
	};

	VkResult result = try_create(create_info);

	if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
	    LowVramDownscale::GetConfig().call_gc_on_oom) {
		LowVramDownscale::RunGcCallback();
		result = try_create(create_info);
	}

	if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
		VkImageCreateInfo emergency_info = *original_create_info;
		LowVramDownscale::ApplyEmergency(emergency_info);

		if (emergency_info.extent.width != original_create_info->extent.width ||
		    emergency_info.extent.height != original_create_info->extent.height) {
			std::fprintf(stderr, "[low-vram-downscale] OOM fallback: %ux%u -> %ux%u\n",
			             original_create_info->extent.width,
			             original_create_info->extent.height,
			             emergency_info.extent.width, emergency_info.extent.height);
			create_info = emergency_info;
			result      = try_create(create_info);
		}
	}

	if (result != VK_SUCCESS) {
		const auto vk_result = static_cast<vk::Result>(result);
		std::fprintf(
		    stderr,
		    "[VMA] vmaCreateImage failed: result=%s (%d), "
		    "extent=%ux%ux%u, format=%d, type=%d, tiling=%d, "
		    "usage=0x%x, flags=0x%x, samples=%u, layers=%u, "
		    "mipLevels=%u, requiredFlags=0x%x\n",
		    vk::to_string(vk_result).c_str(), static_cast<int>(vk_result),
		    image_info.extent.width, image_info.extent.height, image_info.extent.depth,
		    static_cast<int>(image_info.format), static_cast<int>(image_info.imageType),
		    static_cast<int>(image_info.tiling), static_cast<unsigned int>(image_info.usage),
		    static_cast<unsigned int>(image_info.flags),
		    static_cast<unsigned int>(image_info.samples), image_info.arrayLayers,
		    image_info.mipLevels, alloc_info.requiredFlags);
		LogMemoryBudget();
		return false;
	}

	image.image      = native_image;
	image.format     = static_cast<vk::Format>(create_info.format);
	image.image_type = static_cast<vk::ImageType>(create_info.imageType);
	image.extent     = vk::Extent3D {create_info.extent.width, create_info.extent.height,
	                                 create_info.extent.depth};
	image.layers     = create_info.arrayLayers;
	image.mip_levels = create_info.mipLevels;
	image.samples    = static_cast<uint32_t>(create_info.samples);
	image.usage      = static_cast<vk::ImageUsageFlags>(create_info.usage);
	image.flags      = static_cast<vk::ImageCreateFlags>(create_info.flags);
	image.state      = {.layout = static_cast<vk::ImageLayout>(create_info.initialLayout)};
	image.subresource_states.clear();

	std::fprintf(
	    stderr,
	    "[VMA] image created: allocationSize=%llu, memoryType=%u, offset=%llu, "
	    "extent=%ux%ux%u, format=%d, usage=0x%08X%s\n",
	    static_cast<unsigned long long>(allocation_info.size), allocation_info.memoryType,
	    static_cast<unsigned long long>(allocation_info.offset), create_info.extent.width,
	    create_info.extent.height, create_info.extent.depth, static_cast<int>(create_info.format),
	    static_cast<unsigned>(create_info.usage),
	    (create_info.extent.width != image_info.extent.width ||
	     create_info.extent.height != image_info.extent.height)
	        ? " (downscaled)"
	        : "");

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
