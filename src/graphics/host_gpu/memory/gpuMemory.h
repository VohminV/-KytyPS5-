#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORY_GPUMEMORY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORY_GPUMEMORY_H_

#include "common/common.h"
#include "graphics/host_gpu/memory/guestMemory.h"
#include "graphics/host_gpu/memoryTuning.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

enum class GpuResourceType : uint8_t {
	Buffer,
	Texture,
	RenderTarget,
	DepthTarget,
	StorageImage,
	VideoOut,
};

enum class ResourcePlacement : uint8_t { DeviceLocal, HostVisible, Evicted, Uncached };

enum class GpuMemoryPressure : uint8_t { Normal, Elevated, Critical };

[[nodiscard]] const char* GpuResourceTypeName(GpuResourceType type) noexcept;
[[nodiscard]] const char* ResourcePlacementName(ResourcePlacement placement) noexcept;

struct GuestBacking {
	uint64_t           vaddr           = 0;
	uint64_t           size            = 0;
	uint64_t           backing_offset  = 0;
	GuestMemoryKind    kind            = GuestMemoryKind::Unknown;
	int                memory_type     = 0;
	bool               committed       = false;
	char               name[32] {};
};

// Snapshot of the Vulkan allocation behind a guest resource. The manager keeps
// it next to the guest identity so a migration can be reported without
// touching the guest address.
struct VulkanPlacement {
	bool     valid            = false;
	uint64_t device_memory    = 0;
	uint64_t allocation_offset = 0;
	uint64_t allocation_size  = 0;
	uint64_t image            = 0;
	uint64_t buffer           = 0;
	uint32_t memory_type      = UINT32_MAX;
	uint32_t heap_index       = UINT32_MAX;
	uint32_t heap_flags       = 0;
	uint32_t memory_flags     = 0;
	bool     device_local     = false;
	bool     host_visible     = false;

	[[nodiscard]] bool IsSystemMemory() const noexcept {
		return valid && !device_local && host_visible;
	}
};

// State of the caches that translate the guest resource into Vulkan handles.
// A descriptor (or an image view) that outlives the backing it was built from
// is the usual source of a stale-texture read.
struct ResourceCacheState {
	uint64_t image_serial      = 0;
	uint64_t image_id          = 0;
	uint64_t descriptor_serial = 0;
	uint64_t descriptor_view   = 0;
	uint64_t layout            = 0;
	bool     registered        = false;
	bool     cpu_dirty         = false;
	bool     gpu_dirty         = false;
	bool     gpu_modified      = false;
	bool     tracked           = false;
	bool     descriptor_bound  = false;
	bool     suspended         = false;
};

enum class TraceEvent : uint8_t {
	Register,
	Suspend,
	ResumeReuse,
	ResumeDiscard,
	Upload,
	Download,
	GpuWrite,
	GpuRead,
	Descriptor,
	Evict,
	Invalidate,
	Placement,
	Snapshot,
};

[[nodiscard]] const char* TraceEventName(TraceEvent event) noexcept;

// Reads VmaAllocationInfo for a VmaAllocation without exposing VMA in the
// manager header. allocation is a VmaAllocation (VmaAllocation_T*).
[[nodiscard]] VulkanPlacement DescribeVulkanAllocation(const GraphicContext& graphics,
                                                       void*                   allocation);

struct GcWatermarks {
	uint64_t buffer_trigger   = 1ull * 1024 * 1024 * 1024;
	uint64_t buffer_critical  = 2ull * 1024 * 1024 * 1024;
	uint64_t texture_trigger  = 0;
	uint64_t texture_pressure = 1536ull * 1024 * 1024;
	uint64_t texture_critical = 3ull * 1024 * 1024 * 1024;
};

struct BudgetSnapshot {
	bool               can_report        = false;
	bool               discrete          = false;
	uint64_t           device_local_size = 0;
	uint64_t           total_budget      = 0;
	uint64_t           usage             = 0;
	uint64_t           safe_limit        = 0;
	uint64_t           headroom          = 0;
	float              pressure          = 0.0f;
	GpuMemoryPressure  level             = GpuMemoryPressure::Normal;
	GcWatermarks       watermarks;
};

class MemoryBudgetManager final {
public:
	explicit MemoryBudgetManager(GraphicContext& graphics);
	KYTY_CLASS_NO_COPY(MemoryBudgetManager);

	void                                Refresh();
	[[nodiscard]] const BudgetSnapshot& Snapshot() const noexcept { return m_snapshot; }

private:
	void ComputeWatermarks();

	GraphicContext& m_graphics;
	BudgetSnapshot  m_snapshot;
};

class PlacementPolicy final {
public:
	// Placement is decided from observed access pressure, resource lifetime and
	// the live VRAM budget. Render targets, depth surfaces and storage images
	// are always device local; buffers may stay host visible when the CPU writes
	// them and the GPU reads them rarely.
	[[nodiscard]] ResourcePlacement Decide(GpuResourceType type, uint64_t size,
	                                       uint64_t cpu_score, uint64_t gpu_score,
	                                       uint64_t frames_since_access,
	                                       GpuMemoryPressure pressure) const noexcept;
};

struct AddressRange {
	uint64_t begin = 0;
	uint64_t end   = UINT64_MAX;
};

struct FrameMemoryStats {
	uint64_t frame = 0;

	uint64_t guest_objects       = 0;
	uint64_t guest_aliases       = 0;
	uint64_t guest_tracked_bytes = 0;
	uint64_t guest_direct_bytes  = 0;

	uint64_t resident_buffer_bytes  = 0;
	uint64_t resident_image_bytes   = 0;
	uint64_t resident_resource_bytes = 0;
	uint64_t cpu_dirty_bytes        = 0;
	uint64_t gpu_dirty_bytes        = 0;

	uint64_t buffer_upload_bytes   = 0;
	uint64_t texture_upload_bytes  = 0;
	uint64_t buffer_readback_bytes = 0;
	uint64_t texture_readback_bytes = 0;
	uint64_t staging_bytes         = 0;
	uint64_t suspended_bytes       = 0;
	uint64_t cache_reuse_bytes     = 0;
	uint64_t evicted_bytes         = 0;

	uint64_t budget_usage = 0;
	uint64_t budget_total = 0;

	uint32_t buffer_allocate_calls  = 0;
	uint32_t image_allocate_calls   = 0;
	uint32_t upload_calls           = 0;
	uint32_t readback_calls         = 0;
	uint32_t staging_waits          = 0;
	uint32_t eviction_count         = 0;
	uint32_t cache_reuse_hits       = 0;
	uint32_t cache_reuse_rejects    = 0;
	uint32_t suspended_ranges       = 0;
	uint32_t generation_mismatches  = 0;

	uint64_t upload_wait_ns   = 0;
	uint64_t readback_wait_ns = 0;
};

class GpuMemoryManager final {
public:
	explicit GpuMemoryManager(GraphicContext& graphics);
	~GpuMemoryManager();
	KYTY_CLASS_NO_COPY(GpuMemoryManager);

	[[nodiscard]] GuestMemoryRegistry& Guests() noexcept { return m_guests; }
	[[nodiscard]] MemoryBudgetManager& Budget() noexcept { return m_budget; }
	[[nodiscard]] PlacementPolicy&     Placement() noexcept { return m_placement; }

	[[nodiscard]] bool ResolveBacking(uint64_t vaddr, uint64_t size, GuestBacking& backing) const;

	// Diagnostic placement override for the controlled Fallout 4 experiment.
	// Without KYTY_IMAGE_PLACEMENT it always returns DeviceLocal, preserving the
	// unified guest pool and the normal VRAM residency.
	[[nodiscard]] ImageMemoryPlacement DecideImagePlacement(uint64_t vaddr, uint64_t size) const;
	[[nodiscard]] bool                 TraceEnabled(uint64_t vaddr, uint64_t size) const;

	uint64_t RegisterBuffer(uint64_t vaddr, uint64_t size, ResourcePlacement placement);
	void     UnregisterBuffer(uint64_t vaddr, uint64_t size);
	uint64_t RegisterImage(uint64_t vaddr, uint64_t size, GpuResourceType type, bool tiled,
	                       uint64_t storage_bytes = 0);
	void     UnregisterImage(uint64_t vaddr, uint64_t size);

	void RecordVulkanPlacement(uint64_t vaddr, uint64_t size, const VulkanPlacement& placement);
	void RecordCacheState(uint64_t vaddr, uint64_t size, const ResourceCacheState& state);
	void RecordDescriptorBinding(uint64_t vaddr, uint64_t size, uint64_t image_serial,
	                             uint64_t view, uint64_t layout);
	void RecordTrace(TraceEvent event, uint64_t vaddr, uint64_t size, const char* detail = nullptr,
	                 uint64_t bytes = 0, uint64_t fence = 0);

	void MarkCpuDirty(uint64_t vaddr, uint64_t size);
	void MarkGpuDirty(uint64_t vaddr, uint64_t size);
	void MarkRangeUploaded(uint64_t vaddr, uint64_t size, uint64_t bytes);
	void MarkRangeDownloaded(uint64_t vaddr, uint64_t size);
	void RecordUpload(GpuResourceType type, uint64_t bytes, bool staging, uint64_t wait_ns);
	void RecordDownload(GpuResourceType type, uint64_t bytes, uint64_t wait_ns);
	void RecordEviction(GpuResourceType type, uint64_t vaddr, uint64_t size, uint64_t bytes);
	void RecordCacheReuse(uint64_t vaddr, uint64_t size, bool accepted);
	void RecordSuspended(uint64_t vaddr, uint64_t size);

	// Returns false when a cached resource in the range was created from a stale
	// generation. The caller must then drop the cached Vulkan resources.
	[[nodiscard]] bool OnResumeRange(uint64_t vaddr, uint64_t size);
	void               InvalidateGuestBacking(uint64_t offset, uint64_t size,
	                                           GuestMemoryKind kind = GuestMemoryKind::Direct);

	void OnFrameBoundary(uint64_t frame);
	void PrintSummary() const;

private:
	struct ResourceRecord {
		uint64_t           vaddr         = 0;
		uint64_t           size          = 0;
		uint64_t           storage_bytes = 0;
		GpuResourceType    type          = GpuResourceType::Buffer;
		ResourcePlacement  placement     = ResourcePlacement::DeviceLocal;
		GuestObjectKey     object;
		uint64_t           generation         = 0;
		uint64_t           resource_id        = 0;
		uint64_t           placement_generation = 0;
		uint64_t           created_frame      = 0;
		uint64_t           last_frame         = 0;
		uint64_t           cpu_touches        = 0;
		uint64_t           gpu_touches        = 0;
		uint64_t           last_writer_frame  = 0;
		uint64_t           last_writer_vaddr  = 0;
		uint64_t           last_writer_size   = 0;
		uint64_t           last_reader_frame  = 0;
		uint64_t           last_reader_vaddr  = 0;
		uint64_t           last_copy_frame    = 0;
		uint64_t           last_copy_bytes    = 0;
		uint64_t           completion_serial  = 0;
		VulkanPlacement    vulkan;
		ResourceCacheState cache;
		char               last_copy[24] {};
		bool               tiled = false;
	};

	[[nodiscard]] uint64_t RegisterResource(uint64_t vaddr, uint64_t size, GpuResourceType type,
	                                        ResourcePlacement placement, bool tiled,
	                                        uint64_t storage_bytes = 0);
	void UnregisterResource(uint64_t vaddr, uint64_t size);
	ResourceRecord* FindResourceLocked(uint64_t vaddr, uint64_t size);
	void            ChangeRangeLocked(RangeSet& set, uint64_t vaddr, uint64_t size, bool add);
	void            CollectResidentLocked();
	[[nodiscard]] bool FrameLogEnabled() const;
	void            UpdateAtomicPlacementLocked(ResourceRecord& record);
	void            TraceRecordLocked(const ResourceRecord& record, TraceEvent event,
	                                  const char* detail, uint64_t bytes, uint64_t fence);
	void            TraceSnapshotLocked();
	[[nodiscard]] bool MatchesTraceRange(const ResourceRecord& record) const;

	GuestMemoryRegistry                 m_guests;
	MemoryBudgetManager                 m_budget;
	PlacementPolicy                     m_placement;
	mutable std::mutex                  m_mutex;
	std::map<uint64_t, ResourceRecord>  m_resources;
	RangeSet                            m_cpu_dirty;
	RangeSet                            m_gpu_dirty;
	FrameMemoryStats                    m_frame;
	FrameMemoryStats                    m_total;
	uint64_t                            m_resident_buffer_bytes = 0;
	uint64_t                            m_resident_image_bytes  = 0;
	uint64_t                            m_next_frame            = 0;
	uint64_t                            m_next_resource_id      = 1;
	bool                                m_frame_log_enabled     = true;
	bool                                m_frame_log_all         = false;
	mutable bool                        m_shutdown              = false;
	bool                                m_trace_enabled         = false;
	ImageMemoryPlacement                m_image_placement       = ImageMemoryPlacement::DeviceLocal;
	std::vector<AddressRange>           m_trace_ranges;
	std::vector<AddressRange>           m_placement_ranges;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORY_GPUMEMORY_H_
