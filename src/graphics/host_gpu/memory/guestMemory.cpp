#include "graphics/host_gpu/memory/guestMemory.h"

#include "common/assert.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics {

uint64_t GuestMemoryRegistry::EnsureObject(const GuestObjectKey& key, uint64_t size,
                                           int memory_type, const char* name) {
	std::lock_guard lock(m_mutex);
	auto [it, inserted] = m_objects.try_emplace(key);
	auto& object        = it->second;
	if (inserted) {
		object.size        = size;
		object.generation  = m_next_generation++;
		object.memory_type = memory_type;
		if (name != nullptr) {
			std::strncpy(object.name, name, sizeof(object.name) - 1);
		}
		m_stats_dirty = true;
	} else if (size > object.size) {
		object.size   = size;
		m_stats_dirty = true;
	}
	return object.generation;
}

uint64_t GuestMemoryRegistry::GetGeneration(const GuestObjectKey& key) const {
	std::lock_guard lock(m_mutex);
	const auto      found = m_objects.find(key);
	return found != m_objects.end() ? found->second.generation : 0;
}

bool GuestMemoryRegistry::IsGenerationCurrent(const GuestObjectKey& key,
                                              uint64_t              generation) const {
	std::lock_guard lock(m_mutex);
	const auto      found = m_objects.find(key);
	return found != m_objects.end() && found->second.generation == generation;
}

bool GuestMemoryRegistry::HasObject(const GuestObjectKey& key) const {
	std::lock_guard lock(m_mutex);
	return m_objects.contains(key);
}

void GuestMemoryRegistry::AddAlias(const GuestObjectKey& key, uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mutex);
	auto&           aliases = m_aliases[key];
	if (aliases.try_emplace(vaddr, size).second) {
		if (auto object = m_objects.find(key); object != m_objects.end()) {
			object->second.alias_count++;
		}
		m_stats_dirty = true;
	}
}

void GuestMemoryRegistry::RemoveAlias(const GuestObjectKey& key, uint64_t vaddr) {
	std::lock_guard lock(m_mutex);
	const auto      found = m_aliases.find(key);
	if (found == m_aliases.end()) {
		return;
	}
	if (found->second.erase(vaddr) != 0) {
		if (auto object = m_objects.find(key); object != m_objects.end()) {
			EXIT_IF(object->second.alias_count == 0);
			object->second.alias_count--;
		}
		m_stats_dirty = true;
	}
	if (found->second.empty()) {
		m_aliases.erase(found);
	}
}

uint64_t GuestMemoryRegistry::InvalidateBacking(uint64_t offset, uint64_t size,
                                                GuestMemoryKind kind) {
	std::lock_guard lock(m_mutex);
	const auto      end      = size == 0 ? UINT64_MAX : offset + size;
	uint64_t        affected = 0;
	for (auto& [key, object]: m_objects) {
		if (kind != GuestMemoryKind::Unknown && key.kind != kind) {
			continue;
		}
		if (size != 0 && (key.offset >= end || key.offset + object.size <= offset)) {
			continue;
		}
		object.generation = m_next_generation++;
		affected++;
	}
	return affected;
}

uint64_t GuestMemoryRegistry::InvalidateObject(const GuestObjectKey& key) {
	std::lock_guard lock(m_mutex);
	const auto      found = m_objects.find(key);
	if (found == m_objects.end()) {
		return 0;
	}
	found->second.generation = m_next_generation++;
	return found->second.generation;
}

GuestMemoryRegistry::Stats GuestMemoryRegistry::GetStats() const {
	std::lock_guard lock(m_mutex);
	if (m_stats_dirty) {
		RecomputeStatsLocked();
	}
	return m_stats;
}

void GuestMemoryRegistry::RecomputeStatsLocked() const {
	Stats stats;
	for (const auto& [key, object]: m_objects) {
		stats.objects++;
		stats.aliases += object.alias_count;
		stats.tracked_bytes += object.size;
		switch (key.kind) {
			case GuestMemoryKind::Direct: stats.direct_bytes += object.size; break;
			case GuestMemoryKind::Flexible: stats.flexible_bytes += object.size; break;
			case GuestMemoryKind::Pooled: stats.pooled_bytes += object.size; break;
			case GuestMemoryKind::Private: stats.private_bytes += object.size; break;
			case GuestMemoryKind::Unknown: break;
		}
	}
	m_stats       = stats;
	m_stats_dirty = false;
}

} // namespace Libs::Graphics
