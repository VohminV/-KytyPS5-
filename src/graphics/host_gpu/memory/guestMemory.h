#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORY_GUESTMEMORY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORY_GUESTMEMORY_H_

#include "common/common.h"

#include <compare>
#include <cstdint>
#include <map>
#include <mutex>

namespace Libs::Graphics {

// PS5 titles see one unified memory pool. On a discrete host the same physical
// range can be reached through several guest virtual aliases, while the Vulkan
// resources that cache it stay per-alias. The registry deduplicates the backing
// identity and gives every allocation a generation so a recycled physical range
// can never be mistaken for the previous object living there.
enum class GuestMemoryKind : uint8_t { Direct, Flexible, Pooled, Private, Unknown };

struct GuestObjectKey {
	GuestMemoryKind kind   = GuestMemoryKind::Unknown;
	uint64_t        offset = 0;

	auto operator<=>(const GuestObjectKey&) const = default;
};

struct GuestMemoryObject {
	uint64_t size        = 0;
	uint64_t generation  = 0;
	uint64_t alias_count = 0;
	int      memory_type = 0;
	char     name[32] {};
};

class GuestMemoryRegistry final {
public:
	struct Stats {
		uint64_t objects       = 0;
		uint64_t aliases       = 0;
		uint64_t tracked_bytes = 0;
		uint64_t direct_bytes  = 0;
		uint64_t flexible_bytes = 0;
		uint64_t pooled_bytes  = 0;
		uint64_t private_bytes = 0;
	};

	GuestMemoryRegistry() = default;
	KYTY_CLASS_NO_COPY(GuestMemoryRegistry);

	// Creates the object first, returns its stable generation otherwise.
	uint64_t EnsureObject(const GuestObjectKey& key, uint64_t size, int memory_type,
	                      const char* name);
	[[nodiscard]] uint64_t GetGeneration(const GuestObjectKey& key) const;
	[[nodiscard]] bool     IsGenerationCurrent(const GuestObjectKey& key,
	                                           uint64_t              generation) const;
	[[nodiscard]] bool     HasObject(const GuestObjectKey& key) const;

	void AddAlias(const GuestObjectKey& key, uint64_t vaddr, uint64_t size);
	void RemoveAlias(const GuestObjectKey& key, uint64_t vaddr);

	// Called when the guest frees or repurposes physical memory. Every object
	// whose backing offset intersects [offset, offset + size) gets a new
	// generation. A zero size invalidates all objects of the given kind.
	uint64_t InvalidateBacking(uint64_t offset, uint64_t size, GuestMemoryKind kind);
	uint64_t InvalidateObject(const GuestObjectKey& key);

	[[nodiscard]] Stats GetStats() const;

private:
	void RecomputeStatsLocked() const;

	mutable std::mutex                                      m_mutex;
	std::map<GuestObjectKey, GuestMemoryObject>             m_objects;
	std::map<GuestObjectKey, std::map<uint64_t, uint64_t>>  m_aliases;
	mutable Stats                                           m_stats;
	mutable bool                                            m_stats_dirty = true;
	uint64_t                                                m_next_generation = 1;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORY_GUESTMEMORY_H_
