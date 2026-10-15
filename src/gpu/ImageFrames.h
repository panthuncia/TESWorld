#pragma once

// Images through a GPU frame, for tesworld::gpu's clients: laid out in a frame's buffers, their levels uploaded from the
// CPU or written by a kernel, mipped and BC7-encoded there, copied back, and wrapped as DDS. The terrain baker writes
// its level 0 with its own kernel; the texture encoder uploads whole chains.

#include "ContextImpl.h"

#include "tesworld/Texture.h"
#include "tesworld/TextureEncoder.h"

#include <cstdint>
#include <span>
#include <vector>

namespace tesworld::gpu::detail
{
	// One image of a frame: where its levels sit in the scratch buffer (RGBA8), its blocks in the block buffer, and its
	// bytes in the readback and (when uploaded) the upload buffer.
	struct PlacedImage
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		texture::Format format{ texture::Format::BC7 };
		bool uploaded{ false };
		std::vector<std::uint64_t> levelOffsets;
		std::vector<std::uint64_t> blockOffsets;
		std::uint64_t uploadOffset{ 0 };
		std::uint64_t readbackOffset{ 0 };
		std::uint64_t readbackBytes{ 0 };

		// BC7 is encoded on the GPU; other formats come back as RGBA8 levels for the CPU to encode.
		[[nodiscard]] bool GpuBlocks() const { return format == texture::Format::BC7; }
		[[nodiscard]] std::uint32_t Levels() const { return static_cast<std::uint32_t>(levelOffsets.size()); }
		[[nodiscard]] std::uint32_t LevelWidth(std::uint32_t a_level) const { return (std::max)(1u, width >> a_level); }
		[[nodiscard]] std::uint32_t LevelHeight(std::uint32_t a_level) const { return (std::max)(1u, height >> a_level); }
		// RGBA8 bytes of every level.
		[[nodiscard]] std::uint64_t ScratchBytes() const;
	};

	// The buffers of one frame in flight, grown as frames need.
	struct ImageBuffers
	{
		DeviceBuffer scratch;
		DeviceBuffer blocks;
		MappedBuffer upload;
		MappedBuffer readback;
	};

	// Where a frame's images go: Place each, then Ensure the buffers hold them.
	class ImageLayout
	{
	public:
		PlacedImage Place(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_levels, texture::Format a_format, bool a_uploaded);
		[[nodiscard]] std::uint64_t Texels() const { return _scratch / 4; }

	private:
		friend class ImagePrograms;
		std::uint64_t _scratch{ 0 };
		std::uint64_t _blocks{ 0 };
		std::uint64_t _upload{ 0 };
		std::uint64_t _readback{ 0 };
	};

	// The kernels images go through, and what records them. Render thread only.
	class ImagePrograms
	{
	public:
		explicit ImagePrograms(ContextImpl& a_gpu);

		// Grows a_buffers to hold what a_layout placed.
		void Ensure(ImageBuffers& a_buffers, const ImageLayout& a_layout, const char* a_name) const;
		void Release(ImageBuffers& a_buffers) const;

		// Copies an uploaded image's levels into the upload buffer, before the frame is recorded.
		static void Stage(const ImageBuffers& a_buffers, const PlacedImage& a_image, const texture::MipChain& a_chain);

		// In order, in a frame's recording: uploaded levels into the scratch buffer; the levels below each image's top
		// that weren't uploaded; BC7; the bytes into the readback.
		void RecordUploads(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const;
		void RecordMips(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const;
		void RecordBlocks(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const;
		void RecordReadback(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const;

		// Once the frame is done: the image's bytes out of the readback, for Wrap.
		[[nodiscard]] static std::vector<std::uint8_t> Read(const ImageBuffers& a_buffers, const PlacedImage& a_image);

	private:
		ContextImpl& _gpu;
		ComputeProgram _halfSize;
		ComputeProgram _bc7;
	};

	// An image's bytes from Read as a DDS: its BC7 blocks wrapped, or its RGBA8 levels encoded on the CPU. Any thread.
	[[nodiscard]] texture::EncodeResult Wrap(const PlacedImage& a_image, std::span<const std::uint8_t> a_bytes);
}