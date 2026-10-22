#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace Libs::Graphics::MemoryTuning {

// 8 GiB cards (RTX 3050, RX 6600, ...) share their VRAM with the desktop compositor, so the
// percentages below are applied automatically instead of the fixed-GiB heuristic used for
// larger heaps.
inline constexpr uint64_t LOW_VRAM_HEAP_LIMIT = 9ull * 1024 * 1024 * 1024;

struct Limits {
	float soft   = 0.72f;
	float budget = 0.78f;
	float hard   = 0.84f;
};

struct Watermarks {
	uint64_t trigger  = 0;
	uint64_t pressure = 0;
	uint64_t critical = 0;
};

struct Settings {
	Limits   limits;
	bool     limits_overridden  = false;
	bool     staging_overridden = false;
	bool     staging_automatic  = false;
	uint64_t staging_bytes      = 512ull * 1024 * 1024;
};

inline bool ParseFraction(const char* name, float& value) {
	const char* text = std::getenv(name);
	if (text == nullptr || *text == '\0') {
		return false;
	}

	char*       end    = nullptr;
	const float parsed = std::strtof(text, &end);
	if (end == text || *end != '\0' || !std::isfinite(parsed) || parsed <= 0.0f) {
		std::fprintf(stderr, "[memory-tuning] ignoring invalid %s=%s\n", name, text);
		return false;
	}

	value = parsed;
	return true;
}

inline bool ParseStagingMiB(uint64_t& bytes) {
	const char* text = std::getenv("KYTY_STAGING_MIB");
	if (text == nullptr || *text == '\0') {
		return false;
	}

	char*      end  = nullptr;
	const long mib = std::strtol(text, &end, 10);
	if (end == text || *end != '\0' || mib < 16 || mib > 2048) {
		std::fprintf(stderr, "[memory-tuning] ignoring invalid KYTY_STAGING_MIB=%s\n", text);
		return false;
	}

	bytes = static_cast<uint64_t>(mib) * 1024ull * 1024ull;
	return true;
}

inline uint64_t DetectSystemRamMiB() {
#if defined(__linux__)
	std::ifstream meminfo("/proc/meminfo");
	std::string   line;
	while (std::getline(meminfo, line)) {
		if (line.rfind("MemTotal:", 0) != 0) {
			continue;
		}
		unsigned long long kb = 0;
		std::istringstream stream(line.substr(9));
		if (stream >> kb && kb != 0) {
			return static_cast<uint64_t>(kb) / 1024;
		}
	}
#endif
	return 0;
}

inline const Settings& GetSettings() {
	static const Settings settings = [] {
		Settings result;

		const bool soft_set   = ParseFraction("KYTY_VRAM_SOFT", result.limits.soft);
		const bool budget_set = ParseFraction("KYTY_VRAM_BUDGET", result.limits.budget);
		const bool hard_set   = ParseFraction("KYTY_VRAM_HARD", result.limits.hard);

		result.limits_overridden = soft_set || budget_set || hard_set;

		result.limits.soft   = std::clamp(result.limits.soft, 0.0f, 0.99f);
		result.limits.hard   = std::clamp(result.limits.hard, result.limits.soft, 0.99f);
		result.limits.budget =
		    std::clamp(result.limits.budget, result.limits.soft, result.limits.hard);

		result.staging_overridden = ParseStagingMiB(result.staging_bytes);
		const auto ram_mib        = DetectSystemRamMiB();

		// Keep host staging proportional to RAM on 8/16 GiB machines. Larger transfers fall
		// back to a one-shot allocation, so this only trims the persistent ring.
		if (!result.staging_overridden) {
			if (ram_mib != 0 && ram_mib <= 8192) {
				result.staging_bytes     = 48ull * 1024 * 1024;
				result.staging_automatic = true;
			} else if (ram_mib != 0 && ram_mib <= 16384) {
				result.staging_bytes     = 96ull * 1024 * 1024;
				result.staging_automatic = true;
			}
		}

		if (result.limits_overridden) {
			std::fprintf(stderr,
			             "[memory-tuning] VRAM watermarks: soft=%.0f%% budget=%.0f%% "
			             "hard=%.0f%%\n",
			             static_cast<double>(result.limits.soft) * 100.0,
			             static_cast<double>(result.limits.budget) * 100.0,
			             static_cast<double>(result.limits.hard) * 100.0);
		}

		if (result.staging_overridden) {
			std::fprintf(stderr,
			             "[memory-tuning] staging buffer: %llu MiB\n",
			             static_cast<unsigned long long>(result.staging_bytes / (1024ull * 1024ull)));
		} else if (result.staging_automatic) {
			std::fprintf(stderr,
			             "[memory-tuning] staging buffer: %llu MiB (auto for %llu MiB RAM)\n",
			             static_cast<unsigned long long>(result.staging_bytes / (1024ull * 1024ull)),
			             static_cast<unsigned long long>(ram_mib));
		}

		return result;
	}();
	return settings;
}

inline bool IsLowVramHeap(uint64_t device_local_bytes) {
	return device_local_bytes > 0 && device_local_bytes <= LOW_VRAM_HEAP_LIMIT;
}

// True when the 72/78/84 profile should be applied without explicit KYTY_VRAM_* variables.
inline bool IsAutomaticLowVramProfile(uint64_t device_local_bytes) {
	return !GetSettings().limits_overridden && IsLowVramHeap(device_local_bytes);
}

inline uint64_t GetStagingBytes() {
	return GetSettings().staging_bytes;
}

inline Watermarks ComputeWatermarks(const Limits& limits, uint64_t base_bytes,
                                    uint64_t ceiling_bytes) {
	if (base_bytes == 0 || ceiling_bytes == 0) {
		return {};
	}

	const auto scale = [&](float fraction) {
		const long double scaled = static_cast<long double>(base_bytes) * fraction;
		if (scaled >= static_cast<long double>(ceiling_bytes)) {
			return ceiling_bytes;
		}
		return static_cast<uint64_t>(scaled);
	};

	Watermarks watermarks;
	watermarks.trigger  = scale(limits.soft);
	watermarks.pressure = scale(limits.budget);
	watermarks.critical = scale(limits.hard);

	watermarks.trigger  = std::max<uint64_t>(watermarks.trigger, 1);
	watermarks.pressure = std::max(watermarks.pressure, watermarks.trigger);
	watermarks.critical = std::max(watermarks.critical, watermarks.pressure);
	return watermarks;
}

// Returns false when the caller should keep its built-in heuristic (large heaps without
// KYTY_VRAM_* overrides).
inline bool GetWatermarks(uint64_t base_bytes, uint64_t ceiling_bytes, Watermarks& watermarks) {
	const auto& settings = GetSettings();
	if (base_bytes == 0 || ceiling_bytes == 0) {
		return false;
	}
	if (!settings.limits_overridden && !IsLowVramHeap(base_bytes)) {
		return false;
	}
	watermarks = ComputeWatermarks(settings.limits, base_bytes, ceiling_bytes);
	return true;
}

} // namespace Libs::Graphics::MemoryTuning
