#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {

enum ImageSampleFlag : uint32_t {
	ImageSampleFlagOffset = 1u << 4u,
	ImageSampleFlagBias   = 1u << 1u,
	ImageSampleFlagCompare= 1u << 3u,
	ImageSampleFlagLod    = 1u << 0u,
	ImageSampleFlagA16    = 1u << 7u,
};

struct ImageAddressComponent {
	uint32_t bit_offset;
	uint32_t bit_width;
};

// Local mirror of Decoder::ImageAddressComponentLayout for regression testing.
ImageAddressComponent TestImageAddressComponentLayout(uint32_t flags, uint32_t component) {
	const auto width = [flags](uint32_t index) {
		if ((flags & ImageSampleFlagA16) == 0u) return 32u;
		const bool has_offset = (flags & ImageSampleFlagOffset) != 0u;
		const bool has_bias   = (flags & ImageSampleFlagBias) != 0u;
		const bool has_compare= (flags & ImageSampleFlagCompare) != 0u;
		uint32_t named = 0;
		if (has_offset) {
			if (index == named) return 32u;
			++named;
		}
		if (has_bias) {
			if (index == named) return 16u;
			++named;
		}
		if (has_compare) {
			if (index == named) return 32u;
			++named;
		}
		return 16u;
	};
	uint32_t offset = 0;
	for (uint32_t index = 0; index < component; index++) {
		const auto bits = width(index);
		if (bits == 32u) offset = (offset + 31u) & ~31u;
		offset += bits;
	}
	const auto bits = width(component);
	if (bits == 32u) offset = (offset + 31u) & ~31u;
	const auto component_offset = offset;
	offset += bits;
	return {component_offset, bits};
}

uint32_t TestImageAddressDwordCount(uint32_t flags, uint32_t components) {
	if (components == 0u) return 0u;
	const auto last = TestImageAddressComponentLayout(flags, components - 1u);
	return (last.bit_offset + last.bit_width + 31u) / 32u;
}

void Check(bool value, const char *text) {
	if (!value) {
		std::fprintf(stderr, "ImageAddressLayoutTests: failed: %s\n", text);
		std::abort();
	}
}

void TestNonA16All32Bit() {
	const uint32_t flags = ImageSampleFlagOffset | ImageSampleFlagBias |
	                       ImageSampleFlagCompare | ImageSampleFlagLod;
	auto c0 = TestImageAddressComponentLayout(flags, 0u);
	Check(c0.bit_offset == 0u && c0.bit_width == 32u, "non-A16 component 0");
	auto c1 = TestImageAddressComponentLayout(flags, 1u);
	Check(c1.bit_offset == 32u && c1.bit_width == 32u, "non-A16 component 1");
	auto c2 = TestImageAddressComponentLayout(flags, 2u);
	Check(c2.bit_offset == 64u && c2.bit_width == 32u, "non-A16 component 2");
	auto c3 = TestImageAddressComponentLayout(flags, 3u);
	Check(c3.bit_offset == 96u && c3.bit_width == 32u, "non-A16 component 3");
}

void TestA16BiasOnly() {
	const uint32_t flags = ImageSampleFlagA16 | ImageSampleFlagBias;
	auto c0 = TestImageAddressComponentLayout(flags, 0u);
	Check(c0.bit_offset == 0u && c0.bit_width == 16u, "A16 bias-only component 0");
}

void TestA16BiasThenCompare() {
	const uint32_t flags = ImageSampleFlagA16 | ImageSampleFlagBias |
	                       ImageSampleFlagCompare;
	auto c0 = TestImageAddressComponentLayout(flags, 0u);
	Check(c0.bit_offset == 0u && c0.bit_width == 16u, "A16 bias+compare component 0 (bias)");
	auto c1 = TestImageAddressComponentLayout(flags, 1u);
	Check(c1.bit_offset == 32u && c1.bit_width == 32u,
	      "A16 bias+compare component 1 (compare, 32-bit aligned)");
}

void TestA16OffsetThenBias() {
	const uint32_t flags = ImageSampleFlagA16 | ImageSampleFlagOffset |
	                       ImageSampleFlagBias;
	auto c0 = TestImageAddressComponentLayout(flags, 0u);
	Check(c0.bit_offset == 0u && c0.bit_width == 32u, "A16 offset+bias component 0 (offset)");
	auto c1 = TestImageAddressComponentLayout(flags, 1u);
	Check(c1.bit_offset == 32u && c1.bit_width == 16u,
	      "A16 offset+bias component 1 (bias)");
}

void TestA16OffsetBiasCompare() {
	const uint32_t flags = ImageSampleFlagA16 | ImageSampleFlagOffset |
	                       ImageSampleFlagBias | ImageSampleFlagCompare;
	auto c0 = TestImageAddressComponentLayout(flags, 0u);
	Check(c0.bit_offset == 0u && c0.bit_width == 32u,
	      "A16 offset+bias+compare component 0 (offset)");
	auto c1 = TestImageAddressComponentLayout(flags, 1u);
	Check(c1.bit_offset == 32u && c1.bit_width == 16u,
	      "A16 offset+bias+compare component 1 (bias)");
	auto c2 = TestImageAddressComponentLayout(flags, 2u);
	Check(c2.bit_offset == 64u && c2.bit_width == 32u,
	      "A16 offset+bias+compare component 2 (compare)");
}

void TestImageAddressDwordCount() {
	const uint32_t flags = ImageSampleFlagA16 | ImageSampleFlagBias |
	                       ImageSampleFlagCompare;
	Check(TestImageAddressDwordCount(flags, 2u) == 2u, "A16 bias+compare dword count");

	const uint32_t flags2 = ImageSampleFlagOffset | ImageSampleFlagBias |
	                        ImageSampleFlagCompare | ImageSampleFlagLod;
	Check(TestImageAddressDwordCount(flags2, 4u) == 4u, "non-A16 4 components dword count");
}

} // namespace

int main() {
	TestNonA16All32Bit();
	TestA16BiasOnly();
	TestA16BiasThenCompare();
	TestA16OffsetThenBias();
	TestA16OffsetBiasCompare();
	TestImageAddressDwordCount();
	std::printf("ImageAddressLayoutTests: ok\n");
	return 0;
}
