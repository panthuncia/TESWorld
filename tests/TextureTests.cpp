// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Stochastic.h"
#include "tesworld/Texture.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <DirectXTex.h>
// rpcndr.h's.
#undef small

using namespace tesworld;
using namespace tesworld::texture;

namespace
{
	bool Near(float a, float b, float tolerance = 0.02f) { return std::abs(a - b) <= tolerance; }

	Image Checker(std::uint32_t a_size)
	{
		Image image(a_size, a_size);
		for (std::uint32_t y = 0; y < a_size; ++y) {
			for (std::uint32_t x = 0; x < a_size; ++x) {
				const bool on = ((x / 4) + (y / 4)) % 2 == 0;
				image.SetTexel(x, y, on ? Vec4{ 1, 0, 0, 1 } : Vec4{ 0, 0, 1, 1 });
			}
		}
		return image;
	}

	std::vector<std::uint8_t> MarkSrgb(std::vector<std::uint8_t> a_dds, std::uint32_t a_format)
	{
		const auto put = [&](std::size_t offset, std::uint32_t value) { std::memcpy(a_dds.data() + offset, &value, 4); };
		std::uint32_t fourCC; std::memcpy(&fourCC, a_dds.data() + 84, 4);
		if (fourCC != 0x30315844u) {
			const std::array<std::uint32_t, 5> dx10{ a_format, 3, 0, 1, 0 };
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(dx10.data());
			a_dds.insert(a_dds.begin() + 128, bytes, bytes + 20);
			put(80, 4); put(84, 0x30315844u);
			for (std::size_t offset = 88; offset <= 104; offset += 4) put(offset, 0);
		} else put(128, a_format);
		return a_dds;
	}

	void SrgbConversionIsExplicitAndSharedByRangeReads()
	{
		Image image(64, 32);
		for (std::uint32_t y = 0; y < image.height; ++y)
			for (std::uint32_t x = 0; x < image.width; ++x) image.SetTexel(x, y, { 0.5f, 0.03f, 0.9f, 0.75f });
		for (const auto [format, srgb] : { std::pair{Format::RGBA8, 29u}, {Format::BC1, 72u}, {Format::BC3, 78u}, {Format::BC7, 99u} }) {
			const auto linear = EncodeDds(BuildMips(image), format); assert(linear);
			const auto bytes = MarkSrgb(*linear, srgb);
			const auto raw = DecodeDds(bytes); const auto original = DecodeDds(*linear); assert(raw && original && raw->hasAlpha == original->hasAlpha);
			for (std::size_t i = 0; i < raw->levels.size(); ++i) assert(raw->levels[i].rgba == original->levels[i].rgba);
			const DecodeOptions options{ .maxSize = 8, .linearizeSrgb = true, .srgbScale = 0.65f };
			const auto converted = DecodeDds(bytes, options); assert(converted && converted->hasAlpha == raw->hasAlpha);
			const auto unchanged = DecodeDds(*linear, options); assert(unchanged && unchanged->levels.front().rgba == original->levels[3].rgba);
			const ByteReader reader = [&](std::uint64_t offset, std::uint64_t length) -> std::optional<std::vector<std::uint8_t>> {
				const auto from = (std::min<std::uint64_t>)(offset, bytes.size());
				const auto to = from + (std::min<std::uint64_t>)(length, bytes.size() - from);
				return std::vector<std::uint8_t>(bytes.begin() + from, bytes.begin() + to);
			};
			const auto ranged = ReadDds(reader, options); assert(ranged && ranged->levels.size() == converted->levels.size());
			for (std::size_t level = 0; level < converted->levels.size(); ++level) {
				assert(ranged->levels[level].rgba == converted->levels[level].rgba);
				const auto& source = raw->levels[level + 3].rgba;
				const auto& target = converted->levels[level].rgba;
				for (std::size_t i = 0; i < target.size(); ++i) {
					if ((i & 3) == 3) { assert(target[i] == source[i]); continue; }
					const double value = static_cast<double>(source[i]) / 255.0;
					const auto expected = std::lround((value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4)) * 0.65 * 255.0);
					assert(std::abs(static_cast<long>(target[i]) - expected) <= 1);
				}
			}
			assert(!DecodeDds(bytes, DecodeOptions{ .linearizeSrgb = true, .srgbScale = -1.0f }));
		}
		Image contrast(2, 1); contrast.SetTexel(0, 0, {0, 0, 0, 1}); contrast.SetTexel(1, 0, {1, 1, 1, 1});
		MipChain single; single.levels.push_back(std::move(contrast));
		const auto bytes = EncodeDds(single, Format::RGBA8); assert(bytes);
		const auto converted = DecodeDds(MarkSrgb(*bytes, 29), DecodeOptions{ .linearizeSrgb = true });
		assert(converted && Near(converted->levels.back().Texel(0, 0).x, 0.5f));
	}

	void DdsRoundTripsAndDropsLargeLevels()
	{
		const auto chain = BuildMips(Checker(64));
		assert(chain.levels.size() == 7 && chain.levels.back().width == 1);
		// The 1x1 level is the average: half red, half blue.
		const auto average = chain.levels.back().Texel(0, 0);
		assert(Near(average.x, 0.5f) && Near(average.z, 0.5f));

		std::string error;
		for (const auto format : { Format::RGBA8, Format::BC1, Format::BC3, Format::BC7 }) {
			const auto dds = EncodeDds(chain, format, &error);
			assert(dds && error.empty());
			const auto inspected = InspectDds(*dds, &error);
			assert(inspected && inspected->width == 64 && inspected->height == 64 && inspected->mipLevels == 7);
			assert(inspected->fullMipChain && !inspected->cube && inspected->arraySize == 1);
			assert(inspected->compressed == (format != Format::RGBA8));
			assert(!InspectDds(std::span<const std::uint8_t>(*dds).first(dds->size() - 1), &error));
			error.clear();
			const auto decoded = DecodeDds(*dds, {}, &error);
			assert(decoded && decoded->levels.size() == 7 && decoded->Width() == 64);
			const auto texel = decoded->levels.front().Texel(1, 1);
			assert(Near(texel.x, 1.0f, 0.05f) && Near(texel.z, 0.0f, 0.05f));
		}

		const auto dds = EncodeDds(chain, Format::RGBA8);
		const auto small = DecodeDds(*dds, DecodeOptions{ .maxSize = 16, .flipGreen = true });
		assert(small && small->Width() == 16 && small->levels.size() == 5);
		// Green flipped: 0 becomes 1.
		assert(Near(small->levels.front().Texel(0, 0).y, 1.0f));
		assert(!DecodeDds(std::vector<std::uint8_t>(32, 7), {}, &error));
	}

	// Reading only the levels kept gives the chain decoding the whole file gives, whatever the format and size kept; and
	// reads far less of a large block-compressed file.
	void ReadingTheKeptLevelsIsDecodingTheWholeFile()
	{
		std::mt19937 random(11);
		Image noise(512, 256);
		for (auto& value : noise.rgba) value = static_cast<std::uint8_t>(random() & 0xFF);
		const auto chain = BuildMips(noise);
		for (const auto format : { Format::RGBA8, Format::BC1, Format::BC3, Format::BC5, Format::BC7 }) {
			const auto dds = EncodeDds(chain, format);
			assert(dds);
			for (const std::uint32_t maxSize : { 0u, 1u, 3u, 64u, 256u, 300u, 512u, 4096u }) {
				const DecodeOptions options{ .maxSize = maxSize, .flipGreen = maxSize == 64 };
				std::uint64_t read = 0;
				const ByteReader reader = [&](std::uint64_t a_offset, std::uint64_t a_length) -> std::optional<std::vector<std::uint8_t>> {
					const auto from = (std::min<std::uint64_t>)(a_offset, dds->size());
					const auto to = from + (std::min<std::uint64_t>)(a_length, dds->size() - from);
					read += to - from;
					return std::vector<std::uint8_t>(dds->begin() + static_cast<std::ptrdiff_t>(from), dds->begin() + static_cast<std::ptrdiff_t>(to));
				};
				const auto whole = DecodeDds(*dds, options);
				const auto part = ReadDds(reader, options);
				assert(whole && part && whole->hasAlpha == part->hasAlpha && whole->levels.size() == part->levels.size());
				for (std::size_t i = 0; i < whole->levels.size(); ++i) assert(whole->levels[i].rgba == part->levels[i].rgba);
				if (format != Format::RGBA8 && maxSize == 64) assert(read * 10 < dds->size());
			}
		}
		const ByteReader none = [](std::uint64_t, std::uint64_t) -> std::optional<std::vector<std::uint8_t>> { return std::nullopt; };
		assert(!ReadDds(none, DecodeOptions{ .maxSize = 64 }));
	}

	// What EncodeDds wrote before it encoded in bands: DirectXTex compressing the whole chain at once.
	std::vector<std::uint8_t> WholeImageDds(const MipChain& a_chain, DXGI_FORMAT a_format, DirectX::TEX_COMPRESS_FLAGS a_flags)
	{
		DirectX::ScratchImage source;
		assert(SUCCEEDED(source.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, a_chain.Width(), a_chain.Height(), 1, a_chain.levels.size())));
		for (std::size_t level = 0; level < a_chain.levels.size(); ++level) {
			const auto& from = a_chain.levels[level];
			const auto* to = source.GetImage(level, 0, 0);
			for (std::uint32_t y = 0; y < from.height; ++y) {
				std::memcpy(to->pixels + y * to->rowPitch, from.rgba.data() + static_cast<std::size_t>(y) * from.width * 4, static_cast<std::size_t>(from.width) * 4);
			}
		}
		DirectX::ScratchImage encoded;
		assert(SUCCEEDED(DirectX::Compress(source.GetImages(), source.GetImageCount(), source.GetMetadata(), a_format, a_flags, DirectX::TEX_THRESHOLD_DEFAULT, encoded)));
		DirectX::Blob blob;
		assert(SUCCEEDED(DirectX::SaveToDDSMemory(encoded.GetImages(), encoded.GetImageCount(), encoded.GetMetadata(), DirectX::DDS_FLAGS_NONE, blob)));
		const auto* bytes = static_cast<const std::uint8_t*>(blob.GetBufferPointer());
		return { bytes, bytes + blob.GetBufferSize() };
	}

	void BandedEncodingIsAWholeImageEncode()
	{
		std::mt19937 random(7);
		const std::pair<DXGI_FORMAT, Format> formats[] = { { DXGI_FORMAT_BC1_UNORM, Format::BC1 }, { DXGI_FORMAT_BC3_UNORM, Format::BC3 },
			{ DXGI_FORMAT_BC5_UNORM, Format::BC5 }, { DXGI_FORMAT_BC7_UNORM, Format::BC7 } };
		// Many bands, a ragged last band, a level smaller than a block, a single texel.
		for (const auto& [width, height] : { std::pair{ 1024u, 600u }, std::pair{ 6u, 10u }, std::pair{ 1u, 1u } }) {
			Image base(width, height);
			for (auto& byte : base.rgba) byte = static_cast<std::uint8_t>(random() & 0xFF);
			for (std::uint32_t y = 0; y < height; ++y) {
				for (std::uint32_t x = 0; x < width; ++x) {
					if ((x / 16 + y / 16) % 3 == 0) base.SetTexel(x, y, { 0.2f, 0.6f, 0.3f, 1.0f });
				}
			}
			const auto chain = BuildMips(std::move(base));
			for (const auto& [dxgi, format] : formats) {
				auto flags = DirectX::TEX_COMPRESS_DEFAULT;
				if (format == Format::BC7) flags |= DirectX::TEX_COMPRESS_BC7_QUICK;
				std::string error;
				const auto dds = EncodeDds(chain, format, &error);
				assert(dds && error.empty());
				assert(*dds == WholeImageDds(chain, dxgi, flags));
			}
		}
	}

	void SamplingWrapsAndPicksLevels()
	{
		const auto chain = BuildMips(Checker(64));
		// Texel centre of (1,1): red. Wrapping one tile over lands on the same texel.
		assert(Near(SampleLevel(chain, { 1.5f / 64, 1.5f / 64 }, 0, Address::Wrap).x, 1.0f));
		assert(Near(SampleLevel(chain, { 1.0f + 1.5f / 64, 1.5f / 64 }, 0, Address::Wrap).x, 1.0f));
		// A footprint of 64 texels picks the 1x1 level.
		assert(Near(EstimateLod(chain, { 1.0f, 0 }, { 0, 1.0f }), 6.0f));
		assert(Near(SampleGrad(chain, { 0.3f, 0.7f }, { 1.0f, 0 }, { 0, 1.0f }, Address::Wrap).x, 0.5f));
		assert(Near(EstimateLod(chain, { 0.001f, 0 }, { 0, 0.001f }), 0.0f));
	}

	void AlphaCoverageHoldsDownTheChain()
	{
		Image leaves(64, 64);
		for (std::uint32_t y = 0; y < 64; ++y) {
			for (std::uint32_t x = 0; x < 64; ++x) {
				leaves.SetTexel(x, y, { 0, 1, 0, (x % 4 == 0) ? 1.0f : 0.0f });
			}
		}
		const auto plain = BuildMips(leaves);
		const auto kept = BuildMips(leaves, 0.5f);
		// A quarter of the texels pass at the top. Box filtering alone fades the 4x4 level to 0.25 alpha, which fails the
		// test everywhere; scaled for coverage, it passes.
		assert(plain.levels[2].Texel(0, 0).w < 0.5f);
		assert(kept.levels[2].Texel(0, 0).w >= 0.5f);
		assert(kept.hasAlpha);
	}

	void ThinCutoutsKeepTheirArea()
	{
		// Ten texels a fifth or a quarter covered: none passes a half threshold, though together they cover two texels'
		// worth. Scaled, the more covered pass first, and all five of them pass together.
		texture::Image image(10, 1);
		for (std::uint32_t x = 0; x < 10; ++x) image.SetTexel(x, 0, { 0, 1, 0, x < 5 ? 0.2f : 0.25f });
		texture::PreserveAlphaCoverage(image, 0.5f, 0.2f);
		int passing = 0;
		for (std::uint32_t x = 0; x < 10; ++x) passing += image.Texel(x, 0).w >= 0.5f ? 1 : 0;
		assert(passing == 5);
	}

	void DilateFillsPadding()
	{
		Image image(4, 1);
		image.SetTexel(0, 0, { 1, 0, 0, 1 });
		std::vector<std::uint8_t> covered{ 1, 0, 0, 0 };
		Dilate(image, covered, 2);
		assert(Near(image.Texel(2, 0).x, 1.0f) && covered[2] == 1 && covered[3] == 0);
	}

	void StochasticTextureMapsBackToTheSource()
	{
		const auto chain = BuildMips(Checker(64));
		const auto stochastic = BuildStochasticTexture(chain, StochasticMode::DecorrelatedColor, 64);
		assert(stochastic.lutHeight == 7 && stochastic.inverseLut.width == 64);
		assert(stochastic.gaussian.Width() == 64);
		// The Gaussian histogram centres on 0.5; the LUT's row 0 maps its extremes back to the source's extremes.
		float mean = 0;
		for (std::size_t i = 0; i < stochastic.gaussian.levels.front().rgba.size(); i += 4) mean += stochastic.gaussian.levels.front().rgba[i] / 255.0f;
		mean /= 64.0f * 64.0f;
		assert(Near(mean, 0.5f, 0.05f));
		const auto low = SampleBilinear(stochastic.inverseLut, { 0.02f, 0.0f }, Address::Clamp);
		const auto high = SampleBilinear(stochastic.inverseLut, { 0.98f, 0.0f }, Address::Clamp);
		const auto toColour = [&](Vec4 c) {
			return stochastic.colorSpaceOrigin + stochastic.colorSpaceVector0 * c.x + stochastic.colorSpaceVector1 * c.y + stochastic.colorSpaceVector2 * c.z;
		};
		// Along the first (principal) axis the two ends are the two colours.
		const auto a = toColour({ low.x, 0.5f, 0.5f, 1 });
		const auto b = toColour({ high.x, 0.5f, 0.5f, 1 });
		assert(std::abs(a.x - b.x) > 0.5f && std::abs(a.z - b.z) > 0.5f);
		assert(Near(static_cast<float>(InverseNormalCdf(0.5)), 0.0f, 1e-6f));
		assert(Near(static_cast<float>(InverseNormalCdf(0.975)), 1.959964f, 1e-3f));
	}
}

int main()
{
	SrgbConversionIsExplicitAndSharedByRangeReads();
	DdsRoundTripsAndDropsLargeLevels();
	ReadingTheKeptLevelsIsDecodingTheWholeFile();
	BandedEncodingIsAWholeImageEncode();
	SamplingWrapsAndPicksLevels();
	AlphaCoverageHoldsDownTheChain();
	DilateFillsPadding();
	ThinCutoutsKeepTheirArea();
	StochasticTextureMapsBackToTheSource();
	std::cout << "tesworld texture tests passed\n";
	return 0;
}