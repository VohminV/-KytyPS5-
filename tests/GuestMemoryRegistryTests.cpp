#include "graphics/host_gpu/memory/guestMemory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::GuestMemoryKind;
using Libs::Graphics::GuestMemoryRegistry;
using Libs::Graphics::GuestObjectKey;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "GuestMemoryRegistryTests: failed: %s\n", text);
		std::abort();
	}
}

void TestGenerations() {
	GuestMemoryRegistry registry;
	const GuestObjectKey direct {GuestMemoryKind::Direct, 0x1'0000};
	const auto           first = registry.EnsureObject(direct, 0x4000, 1, "direct");
	Check(first != 0, "first generation must be nonzero");
	Check(registry.EnsureObject(direct, 0x4000, 1, "direct") == first,
	      "same object keeps its generation");
	Check(registry.GetGeneration(direct) == first, "generation is queryable");
	Check(registry.IsGenerationCurrent(direct, first), "fresh generation is current");

	const GuestObjectKey other {GuestMemoryKind::Direct, 0x2'0000};
	const auto           second = registry.EnsureObject(other, 0x8000, 2, "other");
	Check(second != first, "different objects have different generations");

	Check(registry.InvalidateObject(direct) != first, "invalidate creates a new generation");
	Check(!registry.IsGenerationCurrent(direct, first), "old generation is stale");
	Check(registry.GetGeneration(other) == second, "unrelated object is untouched");
}

void TestAliasesAndStats() {
	GuestMemoryRegistry registry;
	const GuestObjectKey key {GuestMemoryKind::Direct, 0x10'0000};
	const auto           generation = registry.EnsureObject(key, 0x2'0000, 3, "aliased");
	registry.AddAlias(key, 0x7000'0000, 0x2'0000);
	registry.AddAlias(key, 0x7000'0000, 0x2'0000);
	registry.AddAlias(key, 0x7002'0000, 0x2'0000);
	Check(registry.GetStats().aliases == 2, "duplicate aliases are folded");
	Check(registry.GetStats().objects == 1, "aliases share one object");
	Check(registry.GetStats().direct_bytes == 0x2'0000, "direct bytes are tracked");

	registry.RemoveAlias(key, 0x7000'0000);
	Check(registry.GetStats().aliases == 1, "alias removal is counted");
	registry.RemoveAlias(key, 0x7000'0000);
	Check(registry.GetStats().aliases == 1, "missing alias removal is a no-op");
	Check(registry.IsGenerationCurrent(key, generation), "alias changes do not bump generation");
}

void TestInvalidation() {
	GuestMemoryRegistry registry;
	const GuestObjectKey low {GuestMemoryKind::Direct, 0x10'0000};
	const GuestObjectKey high {GuestMemoryKind::Direct, 0x20'0000};
	const GuestObjectKey flexible {GuestMemoryKind::Flexible, 0x10'0000};
	const GuestObjectKey private_object {GuestMemoryKind::Private, 0x10'0000};
	const auto           low_gen      = registry.EnsureObject(low, 0x2'0000, 1, "low");
	const auto           high_gen     = registry.EnsureObject(high, 0x2'0000, 1, "high");
	const auto           flexible_gen = registry.EnsureObject(flexible, 0x4000, 2, "flex");
	const auto           private_gen  = registry.EnsureObject(private_object, 0x4000, 3, "priv");

	Check(registry.InvalidateBacking(0x10'8000, 0x1000, GuestMemoryKind::Direct) == 1,
	      "only the overlapping direct object is invalidated");
	Check(!registry.IsGenerationCurrent(low, low_gen), "overlap invalidates low");
	Check(registry.IsGenerationCurrent(high, high_gen), "non-overlap keeps high");
	Check(registry.IsGenerationCurrent(flexible, flexible_gen), "kind filter keeps flexible");
	Check(registry.IsGenerationCurrent(private_object, private_gen), "kind filter keeps private");

	Check(registry.InvalidateBacking(0, 0, GuestMemoryKind::Direct) == 2,
	      "zero-sized invalidation walks all direct objects");
	Check(!registry.IsGenerationCurrent(high, high_gen), "high is invalidated too");
}

void TestRanges() {
	GuestMemoryRegistry registry;
	const GuestObjectKey key {GuestMemoryKind::Private, 0x4000};
	const auto           generation = registry.EnsureObject(key, 0x4000, 1, "range");
	Check(registry.InvalidateBacking(0x4000, 0x1000, GuestMemoryKind::Direct) == 0,
	      "wrong kind leaves the object alone");
	Check(registry.IsGenerationCurrent(key, generation), "generation survives wrong-kind range");
}

} // namespace

int main() {
	TestGenerations();
	TestAliasesAndStats();
	TestInvalidation();
	TestRanges();
	std::printf("GuestMemoryRegistryTests: ok\n");
	return 0;
}
