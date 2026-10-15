#pragma once

#include "tesworld/Math.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Textures on the CPU: DDS read into 8-bit RGBA mip chains, sampled as a GPU samples them (bilinear, trilinear from a
// UV footprint, wrap or clamp), and encoded back to DDS. Backed by DirectXTex on every OS.
namespace tesworld::texture
{
	// One level: RGBA8, rows top to bottom.
	struct Image
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::vector<std::uint8_t> rgba;

		Image() = default;
		Image(std::uint32_t a_width, std::uint32_t a_height) :
			width(a_width),
			height(a_height),
			rgba(static_cast<std::size_t>(a_width) * a_height * 4, 0)
		{}

		[[nodiscard]] bool Empty() const { return width == 0 || height == 0; }
		[[nodiscard]] Vec4 Texel(std::uint32_t a_x, std::uint32_t a_y) const
		{
			const auto* p = rgba.data() + (static_cast<std::size_t>(a_y) * width + a_x) * 4;
			constexpr float scale = 1.0f / 255.0f;
			return { p[0] * scale, p[1] * scale, p[2] * scale, p[3] * scale };
		}
		void SetTexel(std::uint32_t a_x, std::uint32_t a_y, Vec4 a_value);
	};

	// Levels from the largest down to 1x1.
	struct MipChain
	{
		std::vector<Image> levels;
		bool hasAlpha{ false };

		[[nodiscard]] bool Empty() const { return levels.empty(); }
		[[nodiscard]] std::uint32_t Width() const { return levels.empty() ? 0 : levels.front().width; }
		[[nodiscard]] std::uint32_t Height() const { return levels.empty() ? 0 : levels.front().height; }
		[[nodiscard]] std::size_t Bytes() const;
	};

	struct DecodeOptions
	{
		// Levels larger than this on either side are dropped (0 keeps all), so a bake reading 4K textures at a coarse
		// footprint holds only what it samples.
		std::uint32_t maxSize{ 0 };
		// For normal maps written for OpenGL (green up), as Skyrim's are: green is turned to Direct3D's convention.
		bool flipGreen{ false };
	};

	[[nodiscard]] std::optional<MipChain> DecodeDds(std::span<const std::uint8_t> a_bytes, const DecodeOptions& a_options = {}, std::string* a_error = nullptr);

	// a_length bytes from a_offset of a file (fewer at its end); none when it can't be read.
	using ByteReader = std::function<std::optional<std::vector<std::uint8_t>>(std::uint64_t a_offset, std::uint64_t a_length)>;

	// DecodeDds reading only what it keeps: a block-compressed 2D texture's header, then the levels a_options.maxSize lets
	// through (the smallest, at the file's end), without the larger ones. Anything else is read whole. The chain is
	// DecodeDds's of the whole file.
	[[nodiscard]] std::optional<MipChain> ReadDds(const ByteReader& a_read, const DecodeOptions& a_options = {}, std::string* a_error = nullptr);

	// A box-filtered chain from one image. With a coverage threshold (alpha-tested textures, 0..1), each level's alpha is
	// scaled so the same fraction of texels passes the test as at the top.
	[[nodiscard]] MipChain BuildMips(Image a_base, std::optional<float> a_alphaCoverageThreshold = std::nullopt);
	[[nodiscard]] Image Resize(const Image& a_image, std::uint32_t a_width, std::uint32_t a_height);

	// Scales an alpha-tested image's alpha (by at most a_maxScale) so that a_target of its texels pass a_threshold (0..1):
	// thin cutouts that cover a little of many texels keep their area instead of vanishing under the test.
	void PreserveAlphaCoverage(Image& a_image, float a_threshold, float a_target, float a_maxScale = 4.0f);

	enum class Format
	{
		BC1,
		BC3,
		BC5,
		BC7,
		RGBA8
	};

	[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeDds(const MipChain& a_chain, Format a_format, std::string* a_error = nullptr);
	// A chain's blocks in a block format, laid out as EncodeDdsBlocks takes them: each level's rows of blocks in turn.
	// Levels are encoded in bands of block rows across the cores, with the bytes a whole-image encode gives.
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeBlocks(const MipChain& a_chain, Format a_format, std::string* a_error = nullptr);
	// A DDS around blocks already encoded (a GPU's): a_mipCount levels from a_width x a_height down, each level's rows of
	// 4x4 blocks in turn (at least one block a side). The header is the one EncodeDds writes for the same image.
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeDdsBlocks(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mipCount, Format a_format,
		std::span<const std::uint8_t> a_blocks, std::string* a_error = nullptr);
	// Bytes of one level's blocks.
	[[nodiscard]] std::size_t BlockBytes(std::uint32_t a_width, std::uint32_t a_height, Format a_format);

	// Fills texels a_covered marks 0 from their covered neighbours, a ring per iteration, marking them covered: the
	// padding around atlas charts takes its colours from the chart's edge.
	void Dilate(Image& a_image, std::vector<std::uint8_t>& a_covered, std::uint32_t a_iterations);

	enum class Address
	{
		Wrap,
		Clamp
	};

	[[nodiscard]] Vec4 SampleBilinear(const Image& a_image, Vec2 a_uv, Address a_address);
	// Trilinear between the levels around a_lod (0 is the largest level kept).
	[[nodiscard]] Vec4 SampleLevel(const MipChain& a_chain, Vec2 a_uv, float a_lod, Address a_address);
	// The level of detail a GPU picks for a UV footprint (texel units): half log2 of the larger derivative's length².
	[[nodiscard]] float EstimateLod(const MipChain& a_chain, Vec2 a_duDx, Vec2 a_duDy);
	[[nodiscard]] Vec4 SampleGrad(const MipChain& a_chain, Vec2 a_uv, Vec2 a_duDx, Vec2 a_duDy, Address a_address);
}
