#include "graphics/host_gpu/memory/gpuMemory.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB = 1024ull * 1024ull;
constexpr uint64_t GiB = 1024ull * MiB;

[[nodiscard]] uint64_t RangeBytes(const RangeSet& set) {
	uint64_t bytes = 0;
	set.ForEach([&bytes](uint64_t begin, uint64_t end) { bytes += end - begin; });
	return bytes;
}

[[nodiscard]] GuestMemoryKind KindFromBacking(const Libs::LibKernel::Memory::GuestBackingInfo& info) {
	if (info.is_direct) {
		return GuestMemoryKind::Direct;
	}
	if (info.is_flexible) {
		return GuestMemoryKind::Flexible;
	}
	if (info.is_pooled) {
		return GuestMemoryKind::Pooled;
	}
	if (info.is_committed) {
		return GuestMemoryKind::Private;
	}
	return GuestMemoryKind::Unknown;
}

[[nodiscard]] const char* GuestKindName(GuestMemoryKind kind) noexcept {
	switch (kind) {
		case GuestMemoryKind::Direct: return "direct";
		case GuestMemoryKind::Flexible: return "flexible";
		case GuestMemoryKind::Pooled: return "pooled";
		case GuestMemoryKind::Private: return "private";
		case GuestMemoryKind::Unknown: return "unknown";
	}
	return "unknown";
}

[[nodiscard]] bool ParseAddress(std::string_view text, uint64_t& value) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
		text.remove_prefix(1);
	}
	if (text.empty()) {
		return false;
	}
	std::string copy(text);
	errno        = 0;
	char* end    = nullptr;
	value        = std::strtoull(copy.c_str(), &end, 0);
	return errno == 0 && end != copy.c_str();
}

[[nodiscard]] bool ParseRangeList(const char* text,
                                  std::vector<AddressRange>& ranges) {
	if (text == nullptr || *text == '\0') {
		return false;
	}
	std::string_view input(text);
	while (!input.empty()) {
		const auto split = input.find_first_of(",; ");
		const auto token = input.substr(0, split);
		input.remove_prefix(split == std::string_view::npos ? input.size() : split + 1);
		if (token.empty()) {
			continue;
		}
		uint64_t begin = 0;
		uint64_t end   = UINT64_MAX;
		if (const auto dash = token.find('-'); dash != std::string_view::npos) {
			if (!ParseAddress(token.substr(0, dash), begin) ||
			    !ParseAddress(token.substr(dash + 1), end)) {
				continue;
			}
		} else if (const auto plus = token.find('+'); plus != std::string_view::npos) {
			uint64_t size = 0;
			if (!ParseAddress(token.substr(0, plus), begin) ||
			    !ParseAddress(token.substr(plus + 1), size) || size == 0) {
				continue;
			}
			end = begin + size;
		} else if (!ParseAddress(token, begin)) {
			continue;
		}
		if (end <= begin) {
			continue;
		}
		ranges.push_back({begin, end});
	}
	return !ranges.empty();
}

[[nodiscard]] bool OverlapsRange(uint64_t vaddr, uint64_t size,
                                 const std::vector<AddressRange>& ranges) {
	if (ranges.empty()) {
		return true;
	}
	const auto end = vaddr > UINT64_MAX - size ? UINT64_MAX : vaddr + size;
	for (const auto& range: ranges) {
		if (vaddr < range.end && range.begin < end) {
			return true;
		}
	}
	return false;
}

} // namespace

const char* TraceEventName(TraceEvent event) noexcept {
	switch (event) {
		case TraceEvent::Register: return "register";
		case TraceEvent::Suspend: return "suspend";
		case TraceEvent::ResumeReuse: return "resume-reuse";
		case TraceEvent::ResumeDiscard: return "resume-discard";
		case TraceEvent::Upload: return "upload";
		case TraceEvent::Download: return "download";
		case TraceEvent::GpuWrite: return "gpu-write";
		case TraceEvent::GpuRead: return "gpu-read";
		case TraceEvent::Descriptor: return "descriptor";
		case TraceEvent::Evict: return "evict";
		case TraceEvent::Invalidate: return "invalidate";
		case TraceEvent::Placement: return "placement";
		case TraceEvent::Snapshot: return "snapshot";
	}
	return "unknown";
}

VulkanPlacement DescribeVulkanAllocation(const GraphicContext& graphics, void* allocation) {
	VulkanPlacement placement;
	if (allocation == nullptr || graphics.allocator == nullptr) {
		return placement;
	}
	VmaAllocationInfo info {};
	vmaGetAllocationInfo(graphics.allocator, static_cast<VmaAllocation>(allocation), &info);
	placement.valid             = true;
	placement.device_memory     = static_cast<uint64_t>(
	    reinterpret_cast<uintptr_t>(info.deviceMemory));
	placement.allocation_offset = info.offset;
	placement.allocation_size   = info.size;
	placement.memory_type       = info.memoryType;
	const auto& properties      = graphics.physical_device_memory_properties;
	if (info.memoryType < properties.memoryTypeCount) {
		const auto& type     = properties.memoryTypes[info.memoryType];
		placement.memory_flags = static_cast<uint32_t>(type.propertyFlags);
		placement.device_local =
		    (type.propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) !=
		    vk::MemoryPropertyFlags {};
		placement.host_visible =
		    (type.propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible) !=
		    vk::MemoryPropertyFlags {};
		if (type.heapIndex < properties.memoryHeapCount) {
			placement.heap_index = type.heapIndex;
			placement.heap_flags =
			    static_cast<uint32_t>(properties.memoryHeaps[type.heapIndex].flags);
		}
	}
	return placement;
}

const char* GpuResourceTypeName(GpuResourceType type) noexcept {
	switch (type) {
		case GpuResourceType::Buffer: return "buffer";
		case GpuResourceType::Texture: return "texture";
		case GpuResourceType::RenderTarget: return "rt";
		case GpuResourceType::DepthTarget: return "depth";
		case GpuResourceType::StorageImage: return "storage";
		case GpuResourceType::VideoOut: return "videoout";
	}
	return "unknown";
}

const char* ResourcePlacementName(ResourcePlacement placement) noexcept {
	switch (placement) {
		case ResourcePlacement::DeviceLocal: return "device";
		case ResourcePlacement::HostVisible: return "host";
		case ResourcePlacement::Evicted: return "evicted";
		case ResourcePlacement::Uncached: return "uncached";
	}
	return "unknown";
}

MemoryBudgetManager::MemoryBudgetManager(GraphicContext& graphics): m_graphics(graphics) {
	ComputeWatermarks();
	Refresh();
}

void MemoryBudgetManager::ComputeWatermarks() {
	auto&       wm          = m_snapshot.watermarks;
	const auto& settings    = MemoryTuning::GetSettings();
	m_snapshot.can_report   = m_graphics.CanReportMemoryUsage();
	m_snapshot.discrete     = m_graphics.physical_device_properties.deviceType ==
	                          vk::PhysicalDeviceType::eDiscreteGpu;
	m_snapshot.device_local_size = m_graphics.GetDeviceLocalMemorySize();
	m_snapshot.total_budget      = m_graphics.GetTotalMemoryBudget();
	m_snapshot.safe_limit        = m_snapshot.total_budget;

	if (!m_snapshot.can_report) {
		wm = GcWatermarks {};
		return;
	}

	const auto budget_bytes = m_snapshot.total_budget;
	const auto budget       = static_cast<int64_t>(std::min<uint64_t>(budget_bytes, INT64_MAX));
	auto       base = m_snapshot.discrete ? m_snapshot.device_local_size : budget_bytes;
	if (base == 0) {
		base = budget_bytes;
	}

	MemoryTuning::Watermarks watermarks;
	const char*              profile = "heuristic";
	if (MemoryTuning::GetWatermarks(base, budget_bytes, watermarks)) {
		profile = MemoryTuning::IsAutomaticLowVramProfile(base) ? "auto low-vram" : "env";
		wm.buffer_trigger   = watermarks.trigger;
		wm.buffer_critical  = watermarks.critical;
		wm.texture_trigger  = watermarks.trigger;
		wm.texture_pressure = watermarks.pressure;
		wm.texture_critical = watermarks.critical;
	} else {
		const auto buffer_threshold = std::min(budget, static_cast<int64_t>(8 * GiB));
		const auto buffer_trigger =
		    std::min(budget - 6 * buffer_threshold / 10, budget - static_cast<int64_t>(GiB));
		const auto buffer_critical =
		    std::min(budget - 2 * buffer_threshold / 10, budget - static_cast<int64_t>(GiB / 2));
		wm.buffer_trigger  = static_cast<uint64_t>(std::max<int64_t>(buffer_trigger, GiB));
		wm.buffer_critical = static_cast<uint64_t>(std::max<int64_t>(buffer_critical, 2 * GiB));

		const auto texture_threshold = std::min<int64_t>(budget, static_cast<int64_t>(8 * GiB));
		wm.texture_pressure = static_cast<uint64_t>(std::max<int64_t>(
		    std::min(budget - 6 * texture_threshold / 10, budget - static_cast<int64_t>(GiB)),
		    static_cast<int64_t>(GiB + GiB / 2)));
		wm.texture_critical = static_cast<uint64_t>(std::max<int64_t>(
		    std::min(budget - 2 * texture_threshold / 10,
		             budget - static_cast<int64_t>(GiB / 2)),
		    static_cast<int64_t>(3 * GiB)));
		wm.texture_trigger = static_cast<uint64_t>(
		    std::max<int64_t>((budget - texture_threshold) / 2, 0));
	}

	if (settings.limits_overridden || MemoryTuning::IsAutomaticLowVramProfile(base)) {
		std::fprintf(stderr,
		             "[memory-tuning] GPU memory (%s): buffer trigger=%" PRIu64
		             " critical=%" PRIu64 " MiB, texture trigger=%" PRIu64
		             " pressure=%" PRIu64 " critical=%" PRIu64 " MiB\n",
		             profile, wm.buffer_trigger / MiB, wm.buffer_critical / MiB,
		             wm.texture_trigger / MiB, wm.texture_pressure / MiB,
		             wm.texture_critical / MiB);
	}
}

void MemoryBudgetManager::Refresh() {
	m_snapshot.can_report   = m_graphics.CanReportMemoryUsage();
	m_snapshot.discrete     = m_graphics.physical_device_properties.deviceType ==
	                          vk::PhysicalDeviceType::eDiscreteGpu;
	m_snapshot.device_local_size = m_graphics.GetDeviceLocalMemorySize();
	m_snapshot.total_budget      = m_graphics.GetTotalMemoryBudget();
	if (!m_snapshot.can_report || m_snapshot.total_budget == 0) {
		m_snapshot.usage      = 0;
		m_snapshot.headroom   = 0;
		m_snapshot.pressure   = 0.0f;
		m_snapshot.level      = GpuMemoryPressure::Normal;
		m_snapshot.safe_limit = m_snapshot.total_budget;
		return;
	}

	m_snapshot.usage = m_graphics.GetDeviceMemoryUsage();
	m_snapshot.headroom =
	    m_snapshot.total_budget > m_snapshot.usage ? m_snapshot.total_budget - m_snapshot.usage : 0;
	m_snapshot.safe_limit = m_snapshot.total_budget;
	m_snapshot.pressure =
	    static_cast<float>(static_cast<double>(m_snapshot.usage) /
	                       static_cast<double>(m_snapshot.total_budget));
	m_snapshot.level =
	    m_snapshot.usage >= m_snapshot.watermarks.buffer_critical
	        ? GpuMemoryPressure::Critical
	        : (m_snapshot.usage >= m_snapshot.watermarks.buffer_trigger
	               ? GpuMemoryPressure::Elevated
	               : GpuMemoryPressure::Normal);
}

ResourcePlacement PlacementPolicy::Decide(GpuResourceType type, uint64_t size, uint64_t cpu_score,
                                          uint64_t gpu_score, uint64_t frames_since_access,
                                          GpuMemoryPressure pressure) const noexcept {
	if (type != GpuResourceType::Buffer) {
		return ResourcePlacement::DeviceLocal;
	}
	if (pressure == GpuMemoryPressure::Critical && frames_since_access > 300) {
		return ResourcePlacement::Evicted;
	}
	if (size <= 16 * MiB && cpu_score > 0 && cpu_score >= gpu_score * 4) {
		return ResourcePlacement::HostVisible;
	}
	if (size <= 4 * MiB && gpu_score == 0 && cpu_score > 0) {
		return ResourcePlacement::HostVisible;
	}
	return ResourcePlacement::DeviceLocal;
}

GpuMemoryManager::GpuMemoryManager(GraphicContext& graphics): m_budget(graphics) {
	const char* log = std::getenv("KYTY_MEMORY_FRAME_LOG");
	if (log != nullptr) {
		if (std::strcmp(log, "0") == 0 || std::strcmp(log, "off") == 0 ||
		    std::strcmp(log, "none") == 0) {
			m_frame_log_enabled = false;
		} else if (std::strcmp(log, "all") == 0 || std::strcmp(log, "1") == 0) {
			m_frame_log_all = true;
		}
	}

	const char* trace = std::getenv("KYTY_MEMORY_TRACE");
	if (trace != nullptr && *trace != '\0' && std::strcmp(trace, "0") != 0 &&
	    std::strcmp(trace, "off") != 0 && std::strcmp(trace, "none") != 0) {
		m_trace_enabled = true;
		ParseRangeList(std::getenv("KYTY_MEMORY_TRACE_ADDRS"), m_trace_ranges);
	}

	const char* placement = std::getenv("KYTY_IMAGE_PLACEMENT");
	if (placement != nullptr &&
	    (std::strcmp(placement, "gtt") == 0 || std::strcmp(placement, "host") == 0 ||
	     std::strcmp(placement, "system") == 0)) {
		m_image_placement = ImageMemoryPlacement::GpuVisibleSystem;
		ParseRangeList(std::getenv("KYTY_IMAGE_PLACEMENT_ADDRS"), m_placement_ranges);
	}

	if (m_trace_enabled || m_image_placement != ImageMemoryPlacement::DeviceLocal) {
		std::fprintf(stderr,
		             "[MEMORY-TRACE] enabled=%u addr-filter=%s placement=%s "
		             "placement-filter=%s\n",
		             m_trace_enabled ? 1u : 0u,
		             m_trace_ranges.empty() ? "all" : "set",
		             m_image_placement == ImageMemoryPlacement::GpuVisibleSystem ? "gtt"
		                                                                         : "device",
		             m_placement_ranges.empty() ? "all" : "set");
	}
}

GpuMemoryManager::~GpuMemoryManager() {
	if (!m_shutdown) {
		PrintSummary();
	}
}

bool GpuMemoryManager::ResolveBacking(uint64_t vaddr, uint64_t size,
                                      GuestBacking& backing) const {
	Libs::LibKernel::Memory::GuestBackingInfo info {};
	if (!Libs::LibKernel::Memory::GetGuestBackingInfo(vaddr, size, info)) {
		return false;
	}
	backing.vaddr          = vaddr;
	backing.size           = info.size;
	backing.backing_offset =
	    info.is_direct || info.backing_offset != 0 ? info.backing_offset : info.start;
	backing.kind           = KindFromBacking(info);
	backing.memory_type    = info.memory_type;
	backing.committed      = info.is_committed;
	std::strncpy(backing.name, info.name, sizeof(backing.name) - 1);
	return true;
}

uint64_t GpuMemoryManager::RegisterBuffer(uint64_t vaddr, uint64_t size,
                                          ResourcePlacement placement) {
	return RegisterResource(vaddr, size, GpuResourceType::Buffer, placement, false);
}

void GpuMemoryManager::UnregisterBuffer(uint64_t vaddr, uint64_t size) {
	UnregisterResource(vaddr, size);
}

uint64_t GpuMemoryManager::RegisterImage(uint64_t vaddr, uint64_t size, GpuResourceType type,
                                         bool tiled, uint64_t storage_bytes) {
	return RegisterResource(vaddr, size, type, ResourcePlacement::DeviceLocal, tiled,
	                        storage_bytes);
}

void GpuMemoryManager::UnregisterImage(uint64_t vaddr, uint64_t size) {
	UnregisterResource(vaddr, size);
}

uint64_t GpuMemoryManager::RegisterResource(uint64_t vaddr, uint64_t size, GpuResourceType type,
                                            ResourcePlacement placement, bool tiled,
                                            uint64_t storage_bytes) {
	GuestBacking backing;
	if (!ResolveBacking(vaddr, size, backing)) {
		backing.vaddr          = vaddr;
		backing.size           = size;
		backing.backing_offset = vaddr;
		backing.kind           = GuestMemoryKind::Private;
	}

	std::lock_guard lock(m_mutex);
	if (auto existing = m_resources.find(vaddr); existing != m_resources.end()) {
		m_guests.RemoveAlias(existing->second.object, vaddr);
	}
	const GuestObjectKey key {backing.kind, backing.backing_offset};
	const auto           generation =
	    m_guests.EnsureObject(key, backing.size != 0 ? backing.size : size, backing.memory_type,
	                          backing.name);
	m_guests.AddAlias(key, vaddr, size);

	ResourceRecord record;
	record.vaddr                = vaddr;
	record.size                 = size;
	record.storage_bytes        = storage_bytes != 0 ? storage_bytes : size;
	record.type                 = type;
	record.placement            = placement;
	record.object               = key;
	record.generation           = generation;
	record.resource_id          = m_next_resource_id++;
	record.placement_generation = 1;
	record.created_frame        = m_next_frame;
	record.last_frame           = m_next_frame;
	record.tiled                = tiled;
	std::strncpy(record.last_copy, "none", sizeof(record.last_copy) - 1);
	m_resources[vaddr]          = record;

	if (type == GpuResourceType::Buffer) {
		m_frame.buffer_allocate_calls++;
	} else {
		m_frame.image_allocate_calls++;
	}
	TraceRecordLocked(m_resources[vaddr], TraceEvent::Register, backing.name, size, 0);
	return generation;
}

void GpuMemoryManager::UnregisterResource(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mutex);
	const auto      found = m_resources.find(vaddr);
	if (found == m_resources.end()) {
		return;
	}
	if (size != 0 && found->second.size != size) {
		return;
	}
	TraceRecordLocked(found->second, TraceEvent::Evict, "unregister", found->second.size, 0);
	m_guests.RemoveAlias(found->second.object, vaddr);
	m_resources.erase(found);
}

bool GpuMemoryManager::TraceEnabled(uint64_t vaddr, uint64_t size) const {
	return m_trace_enabled && OverlapsRange(vaddr, size, m_trace_ranges);
}

ImageMemoryPlacement GpuMemoryManager::DecideImagePlacement(uint64_t vaddr,
                                                            uint64_t size) const {
	if (m_image_placement == ImageMemoryPlacement::DeviceLocal) {
		return ImageMemoryPlacement::DeviceLocal;
	}
	return OverlapsRange(vaddr, size, m_placement_ranges)
	           ? ImageMemoryPlacement::GpuVisibleSystem
	           : ImageMemoryPlacement::DeviceLocal;
}

void GpuMemoryManager::UpdateAtomicPlacementLocked(ResourceRecord& record) {
	if (!record.vulkan.valid) {
		return;
	}
	const auto next = record.vulkan.device_local
	                      ? ResourcePlacement::DeviceLocal
	                      : (record.vulkan.host_visible ? ResourcePlacement::HostVisible
	                                                    : ResourcePlacement::Uncached);
	if (next != record.placement) {
		record.placement = next;
		record.placement_generation++;
	}
}

void GpuMemoryManager::RecordVulkanPlacement(uint64_t vaddr, uint64_t size,
                                             const VulkanPlacement& placement) {
	std::lock_guard lock(m_mutex);
	const auto      end = vaddr + size;
	auto            it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		auto& record = it->second;
		if (record.vaddr + record.size <= vaddr) {
			continue;
		}
		const auto previous_type = record.vulkan.memory_type;
		record.vulkan            = placement;
		UpdateAtomicPlacementLocked(record);
		if (m_trace_enabled && MatchesTraceRange(record)) {
			TraceRecordLocked(record, TraceEvent::Placement,
			                  previous_type == placement.memory_type ? "same-type" : "memory-type",
			                  placement.allocation_size, 0);
		}
	}
}

void GpuMemoryManager::RecordCacheState(uint64_t vaddr, uint64_t size,
                                        const ResourceCacheState& state) {
	std::lock_guard lock(m_mutex);
	const auto      end = vaddr + size;
	auto            it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		if (it->second.vaddr + it->second.size > vaddr) {
			it->second.cache = state;
		}
	}
}

void GpuMemoryManager::RecordDescriptorBinding(uint64_t vaddr, uint64_t size,
                                               uint64_t image_serial, uint64_t view,
                                               uint64_t layout) {
	std::lock_guard lock(m_mutex);
	const auto      end = vaddr + size;
	auto            it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		auto& record = it->second;
		if (record.vaddr + record.size <= vaddr) {
			continue;
		}
		record.cache.image_serial      = image_serial;
		record.cache.descriptor_serial = image_serial;
		record.cache.descriptor_view   = view;
		record.cache.layout            = layout;
		record.cache.descriptor_bound  = true;
		record.completion_serial       = m_next_frame;
		TraceRecordLocked(record, TraceEvent::Descriptor, "bind", 0, image_serial);
	}
}

void GpuMemoryManager::RecordTrace(TraceEvent event, uint64_t vaddr, uint64_t size,
                                   const char* detail, uint64_t bytes, uint64_t fence) {
	if (!m_trace_enabled || !TraceEnabled(vaddr, size)) {
		return;
	}
	std::lock_guard lock(m_mutex);
	const auto      end = vaddr + size;
	auto            it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		auto& record = it->second;
		if (record.vaddr + record.size <= vaddr || !MatchesTraceRange(record)) {
			continue;
		}
		switch (event) {
			case TraceEvent::Upload:
			case TraceEvent::Download:
				std::strncpy(record.last_copy, TraceEventName(event),
				             sizeof(record.last_copy) - 1);
				record.last_copy_bytes   = bytes;
				record.last_copy_frame   = m_next_frame;
				record.completion_serial = fence;
				if (event == TraceEvent::Download) {
					record.cache.gpu_dirty = false;
				}
				break;
			case TraceEvent::GpuWrite:
				record.last_writer_vaddr = vaddr;
				record.last_writer_size  = size;
				record.last_writer_frame = m_next_frame;
				record.completion_serial = fence;
				record.cache.gpu_dirty   = true;
				break;
			case TraceEvent::GpuRead:
				record.last_reader_vaddr = vaddr;
				record.last_reader_frame = m_next_frame;
				record.completion_serial = fence;
				break;
			case TraceEvent::Suspend:
				record.cache.suspended = true;
				break;
			case TraceEvent::ResumeReuse:
			case TraceEvent::ResumeDiscard:
				record.cache.suspended = false;
				if (event == TraceEvent::ResumeDiscard) {
					record.cache.registered   = false;
					record.cache.gpu_modified = false;
					record.cache.cpu_dirty    = true;
				}
				break;
			case TraceEvent::Evict:
				record.cache.registered   = false;
				record.cache.gpu_modified = false;
				record.placement          = ResourcePlacement::Evicted;
				break;
			case TraceEvent::Invalidate:
				record.cache.cpu_dirty = true;
				break;
			default: break;
		}
		TraceRecordLocked(record, event, detail, bytes, fence);
	}
}

GpuMemoryManager::ResourceRecord* GpuMemoryManager::FindResourceLocked(uint64_t vaddr,
                                                                       uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		const auto record_end = it->second.vaddr + it->second.size;
		if (record_end > vaddr) {
			return &it->second;
		}
	}
	return nullptr;
}

void GpuMemoryManager::ChangeRangeLocked(RangeSet& set, uint64_t vaddr, uint64_t size, bool add) {
	if (add) {
		set.Add(vaddr, size);
	} else {
		set.Subtract(vaddr, size);
	}
}

void GpuMemoryManager::MarkCpuDirty(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mutex);
	ChangeRangeLocked(m_cpu_dirty, vaddr, size, true);
	const auto end = vaddr + size;
	auto       it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		if (it->second.vaddr + it->second.size > vaddr) {
			it->second.cpu_touches++;
			it->second.cache.cpu_dirty = true;
		}
	}
}

void GpuMemoryManager::MarkGpuDirty(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mutex);
	ChangeRangeLocked(m_gpu_dirty, vaddr, size, true);
	const auto end = vaddr + size;
	auto       it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		if (it->second.vaddr + it->second.size > vaddr) {
			it->second.gpu_touches++;
			it->second.last_frame        = m_next_frame;
			it->second.last_writer_vaddr = vaddr;
			it->second.last_writer_size  = size;
			it->second.last_writer_frame = m_next_frame;
			it->second.cache.gpu_dirty   = true;
		}
	}
}

void GpuMemoryManager::MarkRangeUploaded(uint64_t vaddr, uint64_t size, uint64_t bytes) {
	std::lock_guard lock(m_mutex);
	ChangeRangeLocked(m_cpu_dirty, vaddr, size, false);
	if (auto* resource = FindResourceLocked(vaddr, size); resource != nullptr) {
		resource->last_frame = m_next_frame;
		resource->storage_bytes =
		    std::max<uint64_t>(resource->storage_bytes, resource->size);
		resource->cache.cpu_dirty = false;
		std::strncpy(resource->last_copy, "upload", sizeof(resource->last_copy) - 1);
		resource->last_copy_bytes = bytes;
		resource->last_copy_frame = m_next_frame;
	}
	(void)bytes;
}

void GpuMemoryManager::MarkRangeDownloaded(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mutex);
	ChangeRangeLocked(m_gpu_dirty, vaddr, size, false);
	if (auto* resource = FindResourceLocked(vaddr, size); resource != nullptr) {
		resource->last_frame = m_next_frame;
		resource->cache.gpu_dirty = false;
		std::strncpy(resource->last_copy, "download", sizeof(resource->last_copy) - 1);
	}
}

void GpuMemoryManager::RecordUpload(GpuResourceType type, uint64_t bytes, bool staging,
                                    uint64_t wait_ns) {
	std::lock_guard lock(m_mutex);
	m_frame.upload_calls++;
	m_frame.upload_wait_ns += wait_ns;
	if (staging) {
		m_frame.staging_bytes += bytes;
	}
	if (type == GpuResourceType::Buffer) {
		m_frame.buffer_upload_bytes += bytes;
	} else {
		m_frame.texture_upload_bytes += bytes;
	}
}

void GpuMemoryManager::RecordDownload(GpuResourceType type, uint64_t bytes, uint64_t wait_ns) {
	std::lock_guard lock(m_mutex);
	m_frame.readback_calls++;
	m_frame.readback_wait_ns += wait_ns;
	if (type == GpuResourceType::Buffer) {
		m_frame.buffer_readback_bytes += bytes;
	} else {
		m_frame.texture_readback_bytes += bytes;
	}
}

void GpuMemoryManager::RecordEviction(GpuResourceType type, uint64_t vaddr, uint64_t size,
                                      uint64_t bytes) {
	std::lock_guard lock(m_mutex);
	m_frame.eviction_count++;
	m_frame.evicted_bytes += bytes;
	const auto found = m_resources.find(vaddr);
	if (found != m_resources.end()) {
		if (type == GpuResourceType::Buffer) {
			m_resident_buffer_bytes =
			    m_resident_buffer_bytes >= found->second.storage_bytes
			        ? m_resident_buffer_bytes - found->second.storage_bytes
			        : 0;
		} else {
			m_resident_image_bytes =
			    m_resident_image_bytes >= found->second.storage_bytes
			        ? m_resident_image_bytes - found->second.storage_bytes
			        : 0;
		}
		(void)size;
	}
}

void GpuMemoryManager::RecordCacheReuse(uint64_t vaddr, uint64_t size, bool accepted) {
	std::lock_guard lock(m_mutex);
	if (accepted) {
		m_frame.cache_reuse_hits++;
		m_frame.cache_reuse_bytes += size;
	} else {
		m_frame.cache_reuse_rejects++;
	}
	const auto end = vaddr + size;
	auto       it  = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		if (it->second.vaddr + it->second.size > vaddr) {
			it->second.last_frame = m_next_frame;
		}
	}
}

void GpuMemoryManager::RecordSuspended(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mutex);
	m_frame.suspended_ranges++;
	m_frame.suspended_bytes += size;
	(void)vaddr;
}

bool GpuMemoryManager::OnResumeRange(uint64_t vaddr, uint64_t size) {
	GuestBacking backing;
	const bool   resolved = ResolveBacking(vaddr, size, backing);

	std::lock_guard lock(m_mutex);
	bool            reusable = true;
	const auto      end      = vaddr + size;
	auto            it       = m_resources.upper_bound(vaddr);
	if (it != m_resources.begin()) {
		--it;
	}
	for (; it != m_resources.end() && it->first < end; ++it) {
		auto& resource = it->second;
		if (resource.vaddr + resource.size <= vaddr) {
			continue;
		}
		resource.cache.suspended = false;
		if (!resolved) {
			reusable = false;
			m_frame.generation_mismatches++;
			resource.cache.registered   = false;
			resource.cache.gpu_modified = false;
			resource.cache.cpu_dirty    = true;
			TraceRecordLocked(resource, TraceEvent::ResumeDiscard, "unresolved-backing", 0, 0);
			continue;
		}
		const GuestObjectKey key {backing.kind, backing.backing_offset};
		if (key != resource.object ||
		    !m_guests.IsGenerationCurrent(resource.object, resource.generation)) {
			reusable = false;
			m_frame.generation_mismatches++;
			resource.cache.registered   = false;
			resource.cache.gpu_modified = false;
			resource.cache.cpu_dirty    = true;
			TraceRecordLocked(resource, TraceEvent::ResumeDiscard,
			                  key != resource.object ? "backing-changed" : "generation-stale", 0,
			                  0);
		} else {
			m_frame.cache_reuse_hits++;
			m_frame.cache_reuse_bytes += resource.size;
			resource.last_frame = m_next_frame;
			TraceRecordLocked(resource, TraceEvent::ResumeReuse, "generation-match", 0, 0);
		}
	}
	if (!reusable) {
		m_frame.cache_reuse_rejects++;
	}
	return reusable;
}

void GpuMemoryManager::InvalidateGuestBacking(uint64_t offset, uint64_t size,
                                               GuestMemoryKind kind) {
	std::lock_guard lock(m_mutex);
	(void)m_guests.InvalidateBacking(offset, size, kind);
}

void GpuMemoryManager::CollectResidentLocked() {
	uint64_t buffers = 0;
	uint64_t images  = 0;
	for (const auto& [vaddr, resource]: m_resources) {
		(void)vaddr;
		if (resource.type == GpuResourceType::Buffer) {
			buffers += resource.storage_bytes;
		} else {
			images += resource.storage_bytes;
		}
	}
	m_resident_buffer_bytes = buffers;
	m_resident_image_bytes  = images;
}

bool GpuMemoryManager::FrameLogEnabled() const {
	return m_frame_log_enabled;
}

bool GpuMemoryManager::MatchesTraceRange(const ResourceRecord& record) const {
	return OverlapsRange(record.vaddr, record.size, m_trace_ranges);
}

void GpuMemoryManager::TraceRecordLocked(const ResourceRecord& record, TraceEvent event,
                                         const char* detail, uint64_t bytes, uint64_t fence) {
	if (!m_trace_enabled || !MatchesTraceRange(record)) {
		return;
	}
	const auto& vk       = record.vulkan;
	const auto& cache    = record.cache;
	const auto  residency = vk.valid ? (vk.device_local ? "vram" : "gtt-system")
	                                 : ResourcePlacementName(record.placement);
	std::fprintf(
	    stderr,
	    "[MEMORY-TRACE] frame=%" PRIu64 " event=%s id=%" PRIu64 " type=%s va=0x%016" PRIx64
	    " size=0x%" PRIx64 " guest=%s:0x%" PRIx64 " gen=%" PRIu64 " placement_gen=%" PRIu64
	    " residency=%s suspended=%u vk_mem=0x%016" PRIx64 " vk_off=%" PRIu64
	    " vk_size=%" PRIu64 " vk_image=0x%016" PRIx64 " vk_buffer=0x%016" PRIx64
	    " mtype=%" PRIu32 " heap=%" PRIu32 " heap_flags=0x%x mem_flags=0x%x"
	    " last_copy=%s/%" PRIu64 "@%" PRIu64 " writer=%" PRIu64 ":0x%016" PRIx64 "+0x%" PRIx64
	    " reader=%" PRIu64 ":0x%016" PRIx64 " fence=%" PRIu64
	    " cache={reg=%u serial=%" PRIu64 " id=%" PRIu64 " cpu_dirty=%u gpu_dirty=%u"
	    " gpu_mod=%u tracked=%u layout=%" PRIu64 " desc=%u/%" PRIu64 " view=0x%016" PRIx64
	    "} bytes=%" PRIu64 " arg=%" PRIu64 "%s%s\n",
	    m_next_frame, TraceEventName(event), record.resource_id,
	    GpuResourceTypeName(record.type), record.vaddr, record.size,
	    GuestKindName(record.object.kind), record.object.offset, record.generation,
	    record.placement_generation, residency, cache.suspended ? 1u : 0u, vk.device_memory,
	    vk.allocation_offset, vk.allocation_size, vk.image, vk.buffer, vk.memory_type,
	    vk.heap_index, vk.heap_flags, vk.memory_flags, record.last_copy,
	    record.last_copy_bytes, record.last_copy_frame, record.last_writer_frame,
	    record.last_writer_vaddr, record.last_writer_size, record.last_reader_frame,
	    record.last_reader_vaddr, record.completion_serial, cache.registered ? 1u : 0u,
	    cache.image_serial, cache.image_id, cache.cpu_dirty ? 1u : 0u,
	    cache.gpu_dirty ? 1u : 0u, cache.gpu_modified ? 1u : 0u, cache.tracked ? 1u : 0u,
	    cache.layout, cache.descriptor_bound ? 1u : 0u, cache.descriptor_serial,
	    cache.descriptor_view, bytes, fence, detail != nullptr ? " detail=" : "",
	    detail != nullptr ? detail : "");
}

void GpuMemoryManager::TraceSnapshotLocked() {
	if (!m_trace_enabled || m_trace_ranges.empty()) {
		return;
	}
	for (const auto& [vaddr, record]: m_resources) {
		(void)vaddr;
		TraceRecordLocked(record, TraceEvent::Snapshot, nullptr, 0, 0);
	}
}

void GpuMemoryManager::OnFrameBoundary(uint64_t frame) {
	if (!FrameLogEnabled()) {
		return;
	}
	if (frame < m_next_frame) {
		return;
	}
	m_budget.Refresh();

	std::lock_guard lock(m_mutex);
	CollectResidentLocked();
	const auto guest = m_guests.GetStats();

	m_frame.frame               = frame;
	m_frame.guest_objects        = guest.objects;
	m_frame.guest_aliases        = guest.aliases;
	m_frame.guest_tracked_bytes  = guest.tracked_bytes;
	m_frame.guest_direct_bytes   = guest.direct_bytes;
	m_frame.resident_buffer_bytes = m_resident_buffer_bytes;
	m_frame.resident_image_bytes  = m_resident_image_bytes;
	m_frame.resident_resource_bytes = m_resident_buffer_bytes + m_resident_image_bytes;
	m_frame.cpu_dirty_bytes      = RangeBytes(m_cpu_dirty);
	m_frame.gpu_dirty_bytes      = RangeBytes(m_gpu_dirty);
	m_frame.budget_usage         = m_budget.Snapshot().usage;
	m_frame.budget_total         = m_budget.Snapshot().total_budget;

	const auto add = [](FrameMemoryStats& total, const FrameMemoryStats& value) {
		total.buffer_upload_bytes += value.buffer_upload_bytes;
		total.texture_upload_bytes += value.texture_upload_bytes;
		total.buffer_readback_bytes += value.buffer_readback_bytes;
		total.texture_readback_bytes += value.texture_readback_bytes;
		total.staging_bytes += value.staging_bytes;
		total.suspended_bytes += value.suspended_bytes;
		total.cache_reuse_bytes += value.cache_reuse_bytes;
		total.evicted_bytes += value.evicted_bytes;
		total.buffer_allocate_calls += value.buffer_allocate_calls;
		total.image_allocate_calls += value.image_allocate_calls;
		total.upload_calls += value.upload_calls;
		total.readback_calls += value.readback_calls;
		total.staging_waits += value.staging_waits;
		total.eviction_count += value.eviction_count;
		total.cache_reuse_hits += value.cache_reuse_hits;
		total.cache_reuse_rejects += value.cache_reuse_rejects;
		total.suspended_ranges += value.suspended_ranges;
		total.generation_mismatches += value.generation_mismatches;
		total.upload_wait_ns += value.upload_wait_ns;
		total.readback_wait_ns += value.readback_wait_ns;
	};
	add(m_total, m_frame);

	const auto budget_pct = m_frame.budget_total == 0
	                            ? 0.0
	                            : 100.0 * static_cast<double>(m_frame.budget_usage) /
	                                  static_cast<double>(m_frame.budget_total);
	std::fprintf(
	    stderr,
	    "[MEMORY-FRAME] frame=%" PRIu64 " guest=%" PRIu64 " obj/%" PRIu64 " alias/%" PRIu64
	    " MiB resident=%" PRIu64 " buf+%" PRIu64 " img+%" PRIu64
	    " MiB dirty=%" PRIu64 "/%" PRIu64
	    " B upload=%" PRIu64 " buf+%" PRIu64 " tex x%" PRIu32 " (%" PRIu64
	    " us) readback=%" PRIu64 " buf+%" PRIu64 " tex x%" PRIu32 " (%" PRIu64
	    " us) staging=%" PRIu64 " MiB evict=%" PRIu32 "/%" PRIu64
	    " B reuse=%" PRIu32 "/%" PRIu64 " B reject=%" PRIu32 " suspend=%" PRIu32
	    " gen-mismatch=%" PRIu32 " vram=%" PRIu64 "/%" PRIu64 " MiB (%.1f%%)\n",
	    m_frame.frame, m_frame.guest_objects, m_frame.guest_aliases,
	    m_frame.guest_tracked_bytes / MiB, m_frame.resident_resource_bytes / MiB,
	    m_frame.resident_buffer_bytes / MiB, m_frame.resident_image_bytes / MiB,
	    m_frame.cpu_dirty_bytes, m_frame.gpu_dirty_bytes, m_frame.buffer_upload_bytes / MiB,
	    m_frame.texture_upload_bytes / MiB, m_frame.upload_calls, m_frame.upload_wait_ns / 1000,
	    m_frame.buffer_readback_bytes / MiB, m_frame.texture_readback_bytes / MiB,
	    m_frame.readback_calls, m_frame.readback_wait_ns / 1000, m_frame.staging_bytes / MiB,
	    m_frame.eviction_count, m_frame.evicted_bytes, m_frame.cache_reuse_hits,
	    m_frame.cache_reuse_bytes, m_frame.cache_reuse_rejects, m_frame.suspended_ranges,
	    m_frame.generation_mismatches, m_frame.budget_usage / MiB,
	    m_frame.budget_total / MiB, budget_pct);

	TraceSnapshotLocked();
	m_frame     = FrameMemoryStats {};
	m_next_frame = frame + (m_frame_log_all ? 1 : 60);
}

void GpuMemoryManager::PrintSummary() const {
	m_shutdown = true;
	const auto guest = m_guests.GetStats();
	std::fprintf(stderr,
	             "[MEMORY-SUMMARY] guest objects=%" PRIu64 " aliases=%" PRIu64
	             " tracked=%" PRIu64 " MiB direct=%" PRIu64 " MiB; upload=%" PRIu64
	             "/%" PRIu64 " MiB (%" PRIu32 " calls), readback=%" PRIu64 "/%" PRIu64
	             " MiB (%" PRIu32 " calls), evictions=%" PRIu32 " (%" PRIu64
	             " MiB), cache-reuse=%" PRIu32 " (%" PRIu64 " MiB, %" PRIu32
	             " rejects), suspended=%" PRIu32 " (%" PRIu64 " MiB)\n",
	             guest.objects, guest.aliases, guest.tracked_bytes / MiB,
	             guest.direct_bytes / MiB, m_total.buffer_upload_bytes / MiB,
	             m_total.texture_upload_bytes / MiB, m_total.upload_calls,
	             m_total.buffer_readback_bytes / MiB, m_total.texture_readback_bytes / MiB,
	             m_total.readback_calls, m_total.eviction_count, m_total.evicted_bytes / MiB,
	             m_total.cache_reuse_hits, m_total.cache_reuse_bytes / MiB,
	             m_total.cache_reuse_rejects, m_total.suspended_ranges,
	             m_total.suspended_bytes / MiB);
}

} // namespace Libs::Graphics
