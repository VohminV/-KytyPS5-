#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace LowVramDownscale {

struct Config {
	bool enabled        = false;
	bool auto_enabled   = false;

	// What to downscale.
	bool downscale_color     = true;  // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
	bool downscale_depth     = true;  // VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
	bool downscale_sampled   = true;  // VK_IMAGE_USAGE_SAMPLED_BIT
	bool downscale_storage   = false; // VK_IMAGE_USAGE_STORAGE_BIT
	bool downscale_transient = true;  // VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT

	// Behaviour.
	bool clamp_mips   = true;
	bool align_even   = true;
	bool call_gc_on_oom = true;
	bool only_on_oom  = false;

	// Target: FullHD.
	uint32_t max_width  = 1920;
	uint32_t max_height = 1080;

	// Auto-enable when the largest device-local heap is at most this size.
	// 9 GiB deliberately includes 8 GiB cards.
	VkDeviceSize auto_enable_heap_limit = 9ULL * 1024 * 1024 * 1024;
};

inline Config& GetConfig() {
	static Config cfg;
	return cfg;
}

using GcCallback = void (*)();

inline GcCallback& GetGcCallback() {
	static GcCallback cb = nullptr;
	return cb;
}

inline void SetGcCallback(GcCallback cb) {
	GetGcCallback() = cb;
}

inline void RunGcCallback() {
	if (GetGcCallback() != nullptr) {
		GetGcCallback()();
	}
}

inline bool ParseBool(const char* value, bool default_value) {
	if (value == nullptr) {
		return default_value;
	}

	if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
	    std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
	    std::strcmp(value, "ON") == 0 || std::strcmp(value, "yes") == 0 ||
	    std::strcmp(value, "YES") == 0) {
		return true;
	}

	if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
	    std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
	    std::strcmp(value, "OFF") == 0 || std::strcmp(value, "no") == 0 ||
	    std::strcmp(value, "NO") == 0) {
		return false;
	}

	return default_value;
}

inline bool ParseExtent(const char* value, uint32_t& w, uint32_t& h) {
	if (value == nullptr) {
		return false;
	}

	unsigned int pw = 0;
	unsigned int ph = 0;

	if (std::sscanf(value, "%ux%u", &pw, &ph) == 2 && pw > 0 && ph > 0) {
		w = static_cast<uint32_t>(pw);
		h = static_cast<uint32_t>(ph);
		return true;
	}

	if (std::sscanf(value, "%u", &pw) == 1 && pw > 0) {
		w = static_cast<uint32_t>(pw);
		h = static_cast<uint32_t>(pw);
		return true;
	}

	return false;
}

inline void Init(VmaAllocator allocator) {
	Config cfg;

	const char* mode           = std::getenv("KYTY_DOWNSCALE");
	bool        forced_by_env  = false;

	if (mode != nullptr && std::strcmp(mode, "auto") != 0) {
		forced_by_env = true;
		cfg.enabled   = ParseBool(mode, false);
	}

	cfg.downscale_color     = ParseBool(std::getenv("KYTY_DOWNSCALE_RT"), true);
	cfg.downscale_depth     = ParseBool(std::getenv("KYTY_DOWNSCALE_DEPTH"), true);
	cfg.downscale_sampled   = ParseBool(std::getenv("KYTY_DOWNSCALE_TEXTURES"), true);
	cfg.downscale_storage   = ParseBool(std::getenv("KYTY_DOWNSCALE_STORAGE"), false);
	cfg.downscale_transient = ParseBool(std::getenv("KYTY_DOWNSCALE_TRANSIENT"), true);

	const char* only_on_oom_env = std::getenv("KYTY_DOWNSCALE_ON_OOM_ONLY");

	cfg.clamp_mips      = ParseBool(std::getenv("KYTY_DOWNSCALE_CLAMP_MIPS"), true);
	cfg.align_even      = ParseBool(std::getenv("KYTY_DOWNSCALE_ALIGN_EVEN"), true);
	cfg.call_gc_on_oom  = ParseBool(std::getenv("KYTY_DOWNSCALE_GC"), true);
	cfg.only_on_oom     = ParseBool(only_on_oom_env, false);

	ParseExtent(std::getenv("KYTY_DOWNSCALE_MAX"), cfg.max_width, cfg.max_height);

	if (const char* limit_mb = std::getenv("KYTY_DOWNSCALE_VRAM_LIMIT_MB")) {
		unsigned int mb = 0;
		if (std::sscanf(limit_mb, "%u", &mb) == 1 && mb > 0) {
			cfg.auto_enable_heap_limit = static_cast<VkDeviceSize>(mb) * 1024ULL * 1024ULL;
		}
	}

	const VkPhysicalDeviceMemoryProperties* memory_props = nullptr;
	if (allocator != nullptr) {
		vmaGetMemoryProperties(allocator, &memory_props);
	}

	VkDeviceSize max_device_local_heap = 0;

	if (memory_props != nullptr) {
		for (uint32_t i = 0; i < memory_props->memoryHeapCount; ++i) {
			const VkMemoryHeap& heap = memory_props->memoryHeaps[i];

			if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
				max_device_local_heap = std::max(max_device_local_heap, heap.size);
			}
		}
	}

	const bool small_vram_device = max_device_local_heap > 0 &&
	                               max_device_local_heap <= cfg.auto_enable_heap_limit;

	if (!forced_by_env && small_vram_device) {
		cfg.enabled      = true;
		cfg.auto_enabled = true;
		// Do not shrink images speculatively on auto-detected low-VRAM devices.
		// Guest code keeps using the original extents, so only use the fallback
		// after a real allocation failure.
		if (only_on_oom_env == nullptr) {
			cfg.only_on_oom = true;
		}
	}

	GetConfig() = cfg;

	if (cfg.enabled) {
		std::fprintf(stderr,
		             "[low-vram-downscale] enabled%s target=%ux%u local_heap=%llu MiB "
		             "only_on_oom=%d\n",
		             cfg.auto_enabled ? " (auto 8GB)" : "", cfg.max_width, cfg.max_height,
		             static_cast<unsigned long long>(max_device_local_heap / (1024ULL * 1024ULL)),
		             static_cast<int>(cfg.only_on_oom));
	}
}

inline bool ShouldDownscale(const VkImageCreateInfo& info, const Config& cfg) {
	if (!cfg.enabled) {
		return false;
	}

	if (info.imageType != VK_IMAGE_TYPE_2D) {
		return false;
	}

	// Do not touch MSAA for now to avoid breaking resolve/attachments.
	if (info.samples != VK_SAMPLE_COUNT_1_BIT) {
		return false;
	}

	if (info.extent.depth != 1) {
		return false;
	}

	if (info.extent.width <= cfg.max_width && info.extent.height <= cfg.max_height) {
		return false;
	}

	if ((info.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 && !cfg.downscale_transient) {
		return false;
	}

	if ((info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 && cfg.downscale_color) {
		return true;
	}

	if ((info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 && cfg.downscale_depth) {
		return true;
	}

	if ((info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && cfg.downscale_sampled) {
		return true;
	}

	if ((info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 && cfg.downscale_storage) {
		return true;
	}

	return false;
}

inline VkExtent3D ScaleExtent(const VkExtent3D& extent, const Config& cfg) {
	if (extent.width == 0 || extent.height == 0) {
		return extent;
	}

	const double sx    = static_cast<double>(cfg.max_width) / static_cast<double>(extent.width);
	const double sy    = static_cast<double>(cfg.max_height) / static_cast<double>(extent.height);
	const double scale = std::min(sx, sy);

	if (scale >= 1.0) {
		return extent;
	}

	uint32_t w = static_cast<uint32_t>(
	    std::llround(static_cast<double>(extent.width) * scale));
	uint32_t h = static_cast<uint32_t>(
	    std::llround(static_cast<double>(extent.height) * scale));

	w = std::max(1u, w);
	h = std::max(1u, h);

	if (cfg.align_even) {
		if (w > 1) {
			w = (w + 1u) & ~1u;
		}

		if (h > 1) {
			h = (h + 1u) & ~1u;
		}
	}

	w = std::min(w, cfg.max_width);
	h = std::min(h, cfg.max_height);

	return VkExtent3D {w, h, 1};
}

inline uint32_t ClampMipLevels(uint32_t width, uint32_t height, uint32_t requested_mips) {
	if (requested_mips == 0) {
		return 1;
	}

	if (requested_mips == VK_REMAINING_MIP_LEVELS) {
		return requested_mips;
	}

	uint32_t max_dim  = std::max(width, height);
	uint32_t max_mips = 1;

	while (max_dim > 1) {
		max_dim >>= 1;
		++max_mips;
	}

	return std::min(requested_mips, max_mips);
}

inline void ApplyWithConfig(VkImageCreateInfo& info, const Config& cfg) {
	if (cfg.only_on_oom) {
		return;
	}

	if (!ShouldDownscale(info, cfg)) {
		return;
	}

	const VkExtent3D old_extent = info.extent;

	info.extent = ScaleExtent(info.extent, cfg);

	if (cfg.clamp_mips) {
		info.mipLevels = ClampMipLevels(info.extent.width, info.extent.height, info.mipLevels);
	}

	std::fprintf(stderr,
	             "[low-vram-downscale] %ux%u -> %ux%u format=%d usage=0x%08X mips=%u "
	             "layers=%u flags=0x%08X\n",
	             old_extent.width, old_extent.height, info.extent.width, info.extent.height,
	             static_cast<int>(info.format), static_cast<unsigned>(info.usage), info.mipLevels,
	             info.arrayLayers, static_cast<unsigned>(info.flags));
}

inline void Apply(VkImageCreateInfo& info) {
	ApplyWithConfig(info, GetConfig());
}

// Emergency downscale used after a normal creation attempt already failed
// with VK_ERROR_OUT_OF_DEVICE_MEMORY. Reduces the target by 25%.
inline void ApplyEmergency(VkImageCreateInfo& info) {
	Config cfg = GetConfig();

	cfg.enabled    = true;
	cfg.only_on_oom = false;

	cfg.max_width  = std::max(64u, (cfg.max_width * 3u) / 4u);
	cfg.max_height = std::max(64u, (cfg.max_height * 3u) / 4u);

	ApplyWithConfig(info, cfg);
}

inline VkResult CreateImageDownscaled(VmaAllocator                        allocator,
                                      const VkImageCreateInfo*             pImageCreateInfo,
                                      const VmaAllocationCreateInfo*       pAllocationCreateInfo,
                                      VkImage*                             pImage,
                                      VmaAllocation*                       pAllocation,
                                      VmaAllocationInfo*                   pAllocationInfo = nullptr) {
	if (allocator == nullptr || pImageCreateInfo == nullptr || pImage == nullptr ||
	    pAllocation == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	VkImageCreateInfo create_info = *pImageCreateInfo;

	Apply(create_info);

	VkResult result = vmaCreateImage(allocator, &create_info, pAllocationCreateInfo, pImage,
	                                 pAllocation, pAllocationInfo);

	if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && GetConfig().call_gc_on_oom) {
		RunGcCallback();

		result = vmaCreateImage(allocator, &create_info, pAllocationCreateInfo, pImage,
		                        pAllocation, pAllocationInfo);
	}

	if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
		VkImageCreateInfo emergency_info = *pImageCreateInfo;

		ApplyEmergency(emergency_info);

		if (emergency_info.extent.width != pImageCreateInfo->extent.width ||
		    emergency_info.extent.height != pImageCreateInfo->extent.height) {
			std::fprintf(stderr, "[low-vram-downscale] OOM fallback: %ux%u -> %ux%u\n",
			             pImageCreateInfo->extent.width, pImageCreateInfo->extent.height,
			             emergency_info.extent.width, emergency_info.extent.height);

			result = vmaCreateImage(allocator, &emergency_info, pAllocationCreateInfo, pImage,
			                        pAllocation, pAllocationInfo);
		}
	}

	return result;
}

} // namespace LowVramDownscale
