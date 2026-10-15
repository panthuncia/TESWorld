#include "tesworld/Texture.h"

#include "tesworld/Profiling.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

#include <oneapi/tbb/parallel_for.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <DirectXTex.h>

namespace tesworld::texture
{
	namespace
	{
		std::uint8_t ToByte(float a_value)
		{
			return static_cast<std::uint8_t>(std::clamp(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f), 0L, 255L));
		}

		std::uint32_t Wrap(std::int64_t a_value, std::uint32_t a_size)
		{
			const auto size = static_cast<std::int64_t>(a_size);
			const auto wrapped = a_value % size;
			return static_cast<std::uint32_t>(wrapped < 0 ? wrapped + size : wrapped);
		}

		std::uint32_t Clamp(std::int64_t a_value, std::uint32_t a_size)
		{
			return static_cast<std::uint32_t>(std::clamp<std::int64_t>(a_value, 0, static_cast<std::int64_t>(a_size) - 1));
		}

		Image FromDirectXImage(const DirectX::Image& a_source, bool a_flipGreen)
		{
			Image image(static_cast<std::uint32_t>(a_source.width), static_cast<std::uint32_t>(a_source.height));
			const auto rowBytes = static_cast<std::size_t>(image.width) * 4;
			for (std::uint32_t y = 0; y < image.height; ++y) {
				std::memcpy(image.rgba.data() + y * rowBytes, a_source.pixels + static_cast<std::size_t>(y) * a_source.rowPitch, rowBytes);
			}
			if (a_flipGreen) {
				for (std::size_t i = 1; i < image.rgba.size(); i += 4) {
					image.rgba[i] = static_cast<std::uint8_t>(255 - image.rgba[i]);
				}
			}
			return image;
		}

		Image HalfSize(const Image& a_image)
		{
			Image half((std::max)(1u, a_image.width / 2), (std::max)(1u, a_image.height / 2));
			for (std::uint32_t y = 0; y < half.height; ++y) {
				for (std::uint32_t x = 0; x < half.width; ++x) {
					for (std::uint32_t c = 0; c < 4; ++c) {
						std::uint32_t sum = 0;
						for (std::uint32_t oy = 0; oy < 2; ++oy) {
							for (std::uint32_t ox = 0; ox < 2; ++ox) {
								const auto sx = (std::min)(a_image.width - 1, x * 2 + ox);
								const auto sy = (std::min)(a_image.height - 1, y * 2 + oy);
								sum += a_image.rgba[(static_cast<std::size_t>(sy) * a_image.width + sx) * 4 + c];
							}
						}
						half.rgba[(static_cast<std::size_t>(y) * half.width + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
					}
				}
			}
			return half;
		}

		float Coverage(const Image& a_image, float a_threshold, float a_scale)
		{
			std::size_t passing = 0;
			for (std::size_t i = 3; i < a_image.rgba.size(); i += 4) {
				if (a_image.rgba[i] / 255.0f * a_scale >= a_threshold) {
					++passing;
				}
			}
			return a_image.rgba.empty() ? 0.0f : static_cast<float>(passing) / static_cast<float>(a_image.rgba.size() / 4);
		}

		void ScaleAlphaForCoverage(Image& a_image, float a_threshold, float a_target, float a_maxScale = 4.0f)
		{
			float low = 0.0f;
			float high = a_maxScale;
			for (int i = 0; i < 12; ++i) {
				const float mid = (low + high) * 0.5f;
				if (Coverage(a_image, a_threshold, mid) < a_target) {
					low = mid;
				} else {
					high = mid;
				}
			}
			for (std::size_t i = 3; i < a_image.rgba.size(); i += 4) {
				a_image.rgba[i] = ToByte(a_image.rgba[i] / 255.0f * high);
			}
		}

		DXGI_FORMAT ToDxgi(Format a_format)
		{
			switch (a_format) {
			case Format::BC1: return DXGI_FORMAT_BC1_UNORM;
			case Format::BC3: return DXGI_FORMAT_BC3_UNORM;
			case Format::BC5: return DXGI_FORMAT_BC5_UNORM;
			case Format::BC7: return DXGI_FORMAT_BC7_UNORM;
			default: return DXGI_FORMAT_R8G8B8A8_UNORM;
			}
		}
	}

	void Image::SetTexel(std::uint32_t a_x, std::uint32_t a_y, Vec4 a_value)
	{
		auto* p = rgba.data() + (static_cast<std::size_t>(a_y) * width + a_x) * 4;
		p[0] = ToByte(a_value.x);
		p[1] = ToByte(a_value.y);
		p[2] = ToByte(a_value.z);
		p[3] = ToByte(a_value.w);
	}

	std::size_t MipChain::Bytes() const
	{
		std::size_t bytes = 0;
		for (const auto& level : levels) bytes += level.rgba.size();
		return bytes;
	}

	namespace
	{
		// A texture's chain from its levels (a_level, by index; null for one not read): those a_options.maxSize lets through,
		// else its smallest shrunk to fit; a single level mipped.
		std::optional<MipChain> ChainOf(const DirectX::TexMetadata& a_metadata, const std::function<const DirectX::Image*(std::size_t)>& a_level,
			const DecodeOptions& a_options, std::string* a_error)
		{
			const auto fail = [&](const char* a_message) -> std::optional<MipChain> {
				if (a_error) *a_error = a_message;
				return std::nullopt;
			};
			MipChain chain;
			chain.hasAlpha = DirectX::HasAlpha(a_metadata.format) && a_metadata.format != DXGI_FORMAT_BC1_UNORM;
			const auto convert = [&](const DirectX::Image& a_source) -> std::optional<Image> {
				DirectX::ScratchImage converted;
				HRESULT result;
				if (DirectX::IsCompressed(a_source.format)) {
					result = DirectX::Decompress(a_source, DXGI_FORMAT_R8G8B8A8_UNORM, converted);
				} else if (a_source.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
					return FromDirectXImage(a_source, a_options.flipGreen);
				} else {
					// Not through WIC: it needs COM on Windows, and isn't there elsewhere.
					result = DirectX::Convert(a_source, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_FORCE_NON_WIC, DirectX::TEX_THRESHOLD_DEFAULT, converted);
				}
				if (FAILED(result) || !converted.GetImage(0, 0, 0)) {
					return std::nullopt;
				}
				return FromDirectXImage(*converted.GetImage(0, 0, 0), a_options.flipGreen);
			};

			const auto fits = [&](std::size_t a_width, std::size_t a_height) {
				return a_options.maxSize == 0 || (a_width <= a_options.maxSize && a_height <= a_options.maxSize);
			};
			for (std::size_t level = 0; level < a_metadata.mipLevels; ++level) {
				const auto* source = a_level(level);
				if (!source || !fits(source->width, source->height)) {
					continue;
				}
				auto image = convert(*source);
				if (!image) {
					return fail("can't decode the DDS's format");
				}
				chain.levels.push_back(std::move(*image));
			}

			if (chain.levels.empty()) {
				// Every level is larger than wanted: shrink the smallest.
				const auto* source = a_level(a_metadata.mipLevels - 1);
				auto image = source ? convert(*source) : std::nullopt;
				if (!image) {
					return fail("can't decode the DDS's format");
				}
				const float scale = static_cast<float>(a_options.maxSize) / static_cast<float>((std::max)(image->width, image->height));
				auto resized = Resize(*image, (std::max)(1u, static_cast<std::uint32_t>(image->width * scale)), (std::max)(1u, static_cast<std::uint32_t>(image->height * scale)));
				auto mips = BuildMips(std::move(resized));
				mips.hasAlpha = chain.hasAlpha;
				return mips;
			}
			if (chain.levels.size() == 1 && (chain.levels.front().width > 1 || chain.levels.front().height > 1)) {
				const bool hasAlpha = chain.hasAlpha;
				chain = BuildMips(std::move(chain.levels.front()));
				chain.hasAlpha = hasAlpha;
			}
			return chain;
		}
	}

	std::optional<MipChain> DecodeDds(std::span<const std::uint8_t> a_bytes, const DecodeOptions& a_options, std::string* a_error)
	{
		TW_ZONE("DecodeDds");
		DirectX::TexMetadata metadata{};
		DirectX::ScratchImage scratch;
		if (FAILED(DirectX::LoadFromDDSMemory(a_bytes.data(), a_bytes.size(), DirectX::DDS_FLAGS_NONE, &metadata, scratch))) {
			if (a_error) *a_error = "not a DDS DirectXTex can read";
			return std::nullopt;
		}
		if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D) {
			if (a_error) *a_error = "not a 2D texture";
			return std::nullopt;
		}
		return ChainOf(metadata, [&](std::size_t a_level) { return scratch.GetImage(a_level, 0, 0); }, a_options, a_error);
	}

	std::optional<MipChain> ReadDds(const ByteReader& a_read, const DecodeOptions& a_options, std::string* a_error)
	{
		TW_ZONE("ReadDds");
		const auto whole = [&]() -> std::optional<MipChain> {
			const auto bytes = a_read(0, (std::numeric_limits<std::uint64_t>::max)());
			if (!bytes) {
				if (a_error) *a_error = "can't be read";
				return std::nullopt;
			}
			return DecodeDds(*bytes, a_options, a_error);
		};
		// The magic, the header and DX10's extension.
		constexpr std::uint64_t kHeader = 4 + 124, kDx10 = 20;
		const auto header = a_read(0, kHeader + kDx10);
		if (!header || header->size() < kHeader) return whole();
		DirectX::TexMetadata metadata{};
		if (FAILED(DirectX::GetMetadataFromDDSMemory(header->data(), header->size(), DirectX::DDS_FLAGS_NONE, metadata))) return whole();
		// Only block formats are stored as DirectXTex keeps them, a level after another from the header's end.
		if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.depth != 1 || metadata.IsCubemap() ||
			!DirectX::IsCompressed(metadata.format) || metadata.mipLevels == 0 || a_options.maxSize == 0) {
			return whole();
		}
		// The pixel format's FourCC: "DX10" when the extension follows.
		std::uint32_t fourCC = 0;
		std::memcpy(&fourCC, header->data() + 84, sizeof(fourCC));
		const auto dataStart = kHeader + (fourCC == (0x44u | (0x58u << 8) | (0x31u << 16) | (0x30u << 24)) ? kDx10 : 0);
		struct Level
		{
			std::size_t width;
			std::size_t height;
			std::size_t rowPitch;
			std::size_t slicePitch;
			std::uint64_t offset;
		};
		std::vector<Level> levels(metadata.mipLevels);
		std::uint64_t offset = dataStart;
		for (std::size_t i = 0; i < levels.size(); ++i) {
			auto& level = levels[i];
			level.width = (std::max<std::size_t>)(1, metadata.width >> i);
			level.height = (std::max<std::size_t>)(1, metadata.height >> i);
			if (FAILED(DirectX::ComputePitch(metadata.format, level.width, level.height, level.rowPitch, level.slicePitch))) return whole();
			level.offset = offset;
			offset += level.slicePitch;
		}
		// The first level kept: the first that fits, else the smallest.
		std::size_t first = levels.size() - 1;
		for (std::size_t i = 0; i < levels.size(); ++i) {
			if (levels[i].width <= a_options.maxSize && levels[i].height <= a_options.maxSize) {
				first = i;
				break;
			}
		}
		const auto tail = a_read(levels[first].offset, offset - levels[first].offset);
		if (!tail || tail->size() != offset - levels[first].offset) return whole();
		std::vector<DirectX::Image> images(levels.size());
		for (std::size_t i = first; i < levels.size(); ++i) {
			images[i] = DirectX::Image{ .width = levels[i].width, .height = levels[i].height, .format = metadata.format, .rowPitch = levels[i].rowPitch,
				.slicePitch = levels[i].slicePitch, .pixels = const_cast<std::uint8_t*>(tail->data() + (levels[i].offset - levels[first].offset)) };
		}
		return ChainOf(metadata, [&](std::size_t a_level) { return a_level >= first ? &images[a_level] : nullptr; }, a_options, a_error);
	}

	void PreserveAlphaCoverage(Image& a_image, float a_threshold, float a_target, float a_maxScale)
	{
		ScaleAlphaForCoverage(a_image, a_threshold, a_target, a_maxScale);
	}

	MipChain BuildMips(Image a_base, std::optional<float> a_alphaCoverageThreshold)
	{
		TW_ZONE("BuildMips");
		MipChain chain;
		const float coverage = a_alphaCoverageThreshold ? Coverage(a_base, *a_alphaCoverageThreshold, 1.0f) : 0.0f;
		chain.levels.push_back(std::move(a_base));
		while (chain.levels.back().width > 1 || chain.levels.back().height > 1) {
			auto next = HalfSize(chain.levels.back());
			if (a_alphaCoverageThreshold) {
				ScaleAlphaForCoverage(next, *a_alphaCoverageThreshold, coverage);
			}
			chain.levels.push_back(std::move(next));
		}
		for (std::size_t i = 3; i < chain.levels.front().rgba.size(); i += 4) {
			if (chain.levels.front().rgba[i] != 255) {
				chain.hasAlpha = true;
				break;
			}
		}
		return chain;
	}

	Image Resize(const Image& a_image, std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_image.width == a_width && a_image.height == a_height) {
			return a_image;
		}
		// Halve while that stays above the target (a box filter), then resample bilinearly.
		Image source = a_image;
		while (source.width >= a_width * 2 && source.height >= a_height * 2) {
			source = HalfSize(source);
		}
		Image result(a_width, a_height);
		for (std::uint32_t y = 0; y < a_height; ++y) {
			for (std::uint32_t x = 0; x < a_width; ++x) {
				const Vec2 uv{ (x + 0.5f) / a_width, (y + 0.5f) / a_height };
				result.SetTexel(x, y, SampleBilinear(source, uv, Address::Clamp));
			}
		}
		return result;
	}

	std::optional<std::vector<std::uint8_t>> EncodeBlocks(const MipChain& a_chain, Format a_format, std::string* a_error)
	{
		TW_ZONE_TEXT("EncodeBlocks", "{}x{}", a_chain.Width(), a_chain.Height());
		const auto fail = [&](const char* a_message) -> std::optional<std::vector<std::uint8_t>> {
			if (a_error) *a_error = a_message;
			return std::nullopt;
		};
		if (a_chain.Empty()) {
			return fail("no image");
		}
		if (a_format == Format::RGBA8) {
			return fail("not a block-compressed format");
		}
		// Each level in bands of whole block rows, compressed side by side: a block is encoded from its own texels alone,
		// so the bytes are a whole level's.
		constexpr std::uint32_t kBandRows = 64;
		struct Band
		{
			std::size_t level;
			std::uint32_t firstRow;
			std::uint32_t rows;
			std::size_t offset;
		};
		std::vector<Band> bands;
		std::size_t total = 0;
		for (std::size_t level = 0; level < a_chain.levels.size(); ++level) {
			const auto& image = a_chain.levels[level];
			if (level > 0) {
				const auto& above = a_chain.levels[level - 1];
				if (image.width != (std::max)(1u, above.width / 2) || image.height != (std::max)(1u, above.height / 2)) {
					return fail("mip chain sizes don't halve");
				}
			}
			const auto rowBytes = BlockBytes(image.width, 1, a_format);
			const auto rows = (std::max)(1u, (image.height + 3) / 4);
			for (std::uint32_t row = 0; row < rows; row += kBandRows) {
				const auto count = (std::min)(kBandRows, rows - row);
				bands.push_back({ level, row, count, total + row * rowBytes });
			}
			total += rowBytes * rows;
		}
		std::vector<std::uint8_t> blocks(total);
		std::atomic<bool> failed{ false };
		oneapi::tbb::parallel_for(std::size_t{ 0 }, bands.size(), [&](std::size_t a_band) {
			const auto& band = bands[a_band];
			const auto& image = a_chain.levels[band.level];
			const auto top = band.firstRow * 4;
			const auto height = (std::min)(band.rows * 4, image.height - top);
			DirectX::Image source{};
			source.width = image.width;
			source.height = height;
			source.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			source.rowPitch = static_cast<std::size_t>(image.width) * 4;
			source.slicePitch = source.rowPitch * height;
			source.pixels = const_cast<std::uint8_t*>(image.rgba.data()) + source.rowPitch * top;
			auto flags = DirectX::TEX_COMPRESS_DEFAULT;
			if (a_format == Format::BC7) flags |= DirectX::TEX_COMPRESS_BC7_QUICK;
			DirectX::ScratchImage encoded;
			{
				TW_ZONE("DirectX::Compress");
				if (FAILED(DirectX::Compress(source, ToDxgi(a_format), flags, DirectX::TEX_THRESHOLD_DEFAULT, encoded))) {
					failed = true;
					return;
				}
			}
			const auto* out = encoded.GetImage(0, 0, 0);
			const auto rowBytes = BlockBytes(image.width, 1, a_format);
			for (std::uint32_t row = 0; row < band.rows; ++row) {
				std::memcpy(blocks.data() + band.offset + row * rowBytes, out->pixels + row * out->rowPitch, rowBytes);
			}
		});
		if (failed) {
			return fail("block compression failed");
		}
		return blocks;
	}

	std::optional<std::vector<std::uint8_t>> EncodeDds(const MipChain& a_chain, Format a_format, std::string* a_error)
	{
		TW_ZONE_TEXT("EncodeDds", "{}x{}", a_chain.Width(), a_chain.Height());
		const auto fail = [&](const char* a_message) -> std::optional<std::vector<std::uint8_t>> {
			if (a_error) *a_error = a_message;
			return std::nullopt;
		};
		if (a_chain.Empty()) {
			return fail("no image");
		}
		if (a_format != Format::RGBA8) {
			const auto blocks = EncodeBlocks(a_chain, a_format, a_error);
			if (!blocks) return std::nullopt;
			return EncodeDdsBlocks(a_chain.Width(), a_chain.Height(), static_cast<std::uint32_t>(a_chain.levels.size()), a_format, *blocks, a_error);
		}
		DirectX::ScratchImage source;
		if (FAILED(source.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, a_chain.Width(), a_chain.Height(), 1, a_chain.levels.size()))) {
			return fail("can't allocate the image");
		}
		for (std::size_t level = 0; level < a_chain.levels.size(); ++level) {
			const auto& from = a_chain.levels[level];
			const auto* to = source.GetImage(level, 0, 0);
			if (!to || to->width != from.width || to->height != from.height) {
				return fail("mip chain sizes don't halve");
			}
			for (std::uint32_t y = 0; y < from.height; ++y) {
				std::memcpy(to->pixels + y * to->rowPitch, from.rgba.data() + static_cast<std::size_t>(y) * from.width * 4, static_cast<std::size_t>(from.width) * 4);
			}
		}
		TW_ZONE("SaveToDDSMemory");
		DirectX::Blob blob;
		if (FAILED(DirectX::SaveToDDSMemory(source.GetImages(), source.GetImageCount(), source.GetMetadata(), DirectX::DDS_FLAGS_NONE, blob))) {
			return fail("writing the DDS failed");
		}
		const auto* bytes = static_cast<const std::uint8_t*>(blob.GetBufferPointer());
		return std::vector<std::uint8_t>(bytes, bytes + blob.GetBufferSize());
	}

	std::size_t BlockBytes(std::uint32_t a_width, std::uint32_t a_height, Format a_format)
	{
		const std::size_t blockBytes = a_format == Format::BC1 ? 8 : 16;
		return static_cast<std::size_t>((std::max)(1u, (a_width + 3) / 4)) * (std::max)(1u, (a_height + 3) / 4) * blockBytes;
	}

	std::optional<std::vector<std::uint8_t>> EncodeDdsBlocks(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mipCount, Format a_format,
		std::span<const std::uint8_t> a_blocks, std::string* a_error)
	{
		const auto fail = [&](const char* a_message) -> std::optional<std::vector<std::uint8_t>> {
			if (a_error) *a_error = a_message;
			return std::nullopt;
		};
		if (a_format == Format::RGBA8 || a_width == 0 || a_height == 0 || a_mipCount == 0) {
			return fail("not a block-compressed image");
		}
		DirectX::ScratchImage image;
		if (FAILED(image.Initialize2D(ToDxgi(a_format), a_width, a_height, 1, a_mipCount))) {
			return fail("can't allocate the image");
		}
		std::size_t offset = 0;
		for (std::size_t level = 0; level < a_mipCount; ++level) {
			const auto* to = image.GetImage(level, 0, 0);
			if (!to) {
				return fail("mip chain sizes don't halve");
			}
			const auto width = static_cast<std::uint32_t>(to->width);
			const auto height = static_cast<std::uint32_t>(to->height);
			const auto rowBytes = BlockBytes(width, 1, a_format);
			const auto rows = (std::max)(1u, (height + 3) / 4);
			if (offset + rowBytes * rows > a_blocks.size()) {
				return fail("too few blocks for the image");
			}
			for (std::uint32_t row = 0; row < rows; ++row) {
				std::memcpy(to->pixels + row * to->rowPitch, a_blocks.data() + offset + row * rowBytes, rowBytes);
			}
			offset += rowBytes * rows;
		}
		DirectX::Blob blob;
		if (FAILED(DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS_NONE, blob))) {
			return fail("writing the DDS failed");
		}
		const auto* bytes = static_cast<const std::uint8_t*>(blob.GetBufferPointer());
		return std::vector<std::uint8_t>(bytes, bytes + blob.GetBufferSize());
	}

	void Dilate(Image& a_image, std::vector<std::uint8_t>& a_covered, std::uint32_t a_iterations)
	{
		std::vector<std::uint8_t> next;
		for (std::uint32_t iteration = 0; iteration < a_iterations; ++iteration) {
			next = a_covered;
			bool changed = false;
			for (std::uint32_t y = 0; y < a_image.height; ++y) {
				for (std::uint32_t x = 0; x < a_image.width; ++x) {
					const auto index = static_cast<std::size_t>(y) * a_image.width + x;
					if (a_covered[index]) continue;
					std::uint32_t sum[4]{};
					std::uint32_t count = 0;
					for (int dy = -1; dy <= 1; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							const auto nx = static_cast<std::int64_t>(x) + dx;
							const auto ny = static_cast<std::int64_t>(y) + dy;
							if (nx < 0 || ny < 0 || nx >= a_image.width || ny >= a_image.height) continue;
							const auto neighbour = static_cast<std::size_t>(ny) * a_image.width + static_cast<std::size_t>(nx);
							if (!a_covered[neighbour]) continue;
							for (int c = 0; c < 4; ++c) sum[c] += a_image.rgba[neighbour * 4 + c];
							++count;
						}
					}
					if (count == 0) continue;
					for (int c = 0; c < 4; ++c) a_image.rgba[index * 4 + c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
					next[index] = 1;
					changed = true;
				}
			}
			a_covered.swap(next);
			if (!changed) break;
		}
	}

	Vec4 SampleBilinear(const Image& a_image, Vec2 a_uv, Address a_address)
	{
		if (a_image.Empty()) {
			return { 0, 0, 0, 0 };
		}
		const float fx = a_uv.x * a_image.width - 0.5f;
		const float fy = a_uv.y * a_image.height - 0.5f;
		const auto x0 = static_cast<std::int64_t>(std::floor(fx));
		const auto y0 = static_cast<std::int64_t>(std::floor(fy));
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const auto address = [&](std::int64_t a_value, std::uint32_t a_size) {
			return a_address == Address::Wrap ? Wrap(a_value, a_size) : Clamp(a_value, a_size);
		};
		const auto ax0 = address(x0, a_image.width), ax1 = address(x0 + 1, a_image.width);
		const auto ay0 = address(y0, a_image.height), ay1 = address(y0 + 1, a_image.height);
		const auto t00 = a_image.Texel(ax0, ay0), t10 = a_image.Texel(ax1, ay0);
		const auto t01 = a_image.Texel(ax0, ay1), t11 = a_image.Texel(ax1, ay1);
		const auto lerp = [](Vec4 a, Vec4 b, float t) {
			return Vec4{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
		};
		return lerp(lerp(t00, t10, tx), lerp(t01, t11, tx), ty);
	}

	Vec4 SampleLevel(const MipChain& a_chain, Vec2 a_uv, float a_lod, Address a_address)
	{
		if (a_chain.Empty()) {
			return { 0, 0, 0, 0 };
		}
		const float maxLevel = static_cast<float>(a_chain.levels.size() - 1);
		const float lod = std::clamp(a_lod, 0.0f, maxLevel);
		const auto low = static_cast<std::size_t>(std::floor(lod));
		const auto high = (std::min)(low + 1, a_chain.levels.size() - 1);
		const float t = lod - static_cast<float>(low);
		const auto a = SampleBilinear(a_chain.levels[low], a_uv, a_address);
		if (t <= 0.0f || high == low) {
			return a;
		}
		const auto b = SampleBilinear(a_chain.levels[high], a_uv, a_address);
		return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
	}

	float EstimateLod(const MipChain& a_chain, Vec2 a_duDx, Vec2 a_duDy)
	{
		const float width = static_cast<float>((std::max)(1u, a_chain.Width()));
		const float height = static_cast<float>((std::max)(1u, a_chain.Height()));
		const float dx = (a_duDx.x * width) * (a_duDx.x * width) + (a_duDx.y * height) * (a_duDx.y * height);
		const float dy = (a_duDy.x * width) * (a_duDy.x * width) + (a_duDy.y * height) * (a_duDy.y * height);
		return (std::max)(0.0f, 0.5f * std::log2((std::max)((std::max)(dx, dy), 1.0e-8f)));
	}

	Vec4 SampleGrad(const MipChain& a_chain, Vec2 a_uv, Vec2 a_duDx, Vec2 a_duDy, Address a_address)
	{
		return SampleLevel(a_chain, a_uv, EstimateLod(a_chain, a_duDx, a_duDy), a_address);
	}
}
