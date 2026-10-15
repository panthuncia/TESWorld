#include "ImageFrames.h"

#include "Layout.h"

#include "tesworld/Profiling.h"

#include <algorithm>
#include <cstring>

namespace tesworld::gpu::detail
{
	namespace layout = gpu::layout;

	std::uint64_t PlacedImage::ScratchBytes() const
	{
		std::uint64_t bytes = 0;
		for (std::uint32_t level = 0; level < Levels(); ++level) bytes += static_cast<std::uint64_t>(LevelWidth(level)) * LevelHeight(level) * 4;
		return bytes;
	}

	PlacedImage ImageLayout::Place(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_levels, texture::Format a_format, bool a_uploaded)
	{
		PlacedImage image{ .width = a_width, .height = a_height, .format = a_format, .uploaded = a_uploaded };
		const auto firstBlock = _blocks;
		for (std::uint32_t level = 0; level < a_levels; ++level) {
			const auto width = image.LevelWidth(level);
			const auto height = image.LevelHeight(level);
			image.levelOffsets.push_back(_scratch);
			_scratch += static_cast<std::uint64_t>(width) * height * 4;
			if (image.GpuBlocks()) {
				image.blockOffsets.push_back(_blocks);
				_blocks += texture::BlockBytes(width, height, a_format);
			}
		}
		if (a_uploaded) {
			image.uploadOffset = _upload;
			_upload += image.ScratchBytes();
		}
		image.readbackOffset = _readback;
		image.readbackBytes = image.GpuBlocks() ? _blocks - firstBlock : image.ScratchBytes();
		_readback += image.readbackBytes;
		return image;
	}

	ImagePrograms::ImagePrograms(ContextImpl& a_gpu) :
		_gpu(a_gpu),
		_halfSize(a_gpu.CreateProgram("HalfSize", "HalfSizeCS", sizeof(layout::HalfSizeConstants) / 4)),
		_bc7(a_gpu.CreateProgram("Bc7Mode6", "Bc7CS", sizeof(layout::Bc7Constants) / 4))
	{}

	void ImagePrograms::Ensure(ImageBuffers& a_buffers, const ImageLayout& a_layout, const char* a_name) const
	{
		// Half again what's asked, so frames of about the same size don't reallocate.
		const auto grown = [](std::uint64_t a_bytes) { return (std::max<std::uint64_t>)(a_bytes, 16) * 3 / 2; };
		if (a_buffers.scratch.bytes < a_layout._scratch) {
			_gpu.Release(a_buffers.scratch);
			a_buffers.scratch = _gpu.CreateDeviceBuffer(grown(a_layout._scratch), false, true, a_name);
		}
		if (a_buffers.blocks.bytes < (std::max<std::uint64_t>)(a_layout._blocks, 16)) {
			_gpu.Release(a_buffers.blocks);
			a_buffers.blocks = _gpu.CreateDeviceBuffer(grown(a_layout._blocks), false, true, a_name);
		}
		if (a_layout._upload > 0 && a_buffers.upload.bytes < a_layout._upload) {
			a_buffers.upload = _gpu.CreateMapped(grown(a_layout._upload), rhi::HeapType::Upload, a_name);
		}
		if (a_buffers.readback.bytes < a_layout._readback) {
			a_buffers.readback = _gpu.CreateMapped(grown(a_layout._readback), rhi::HeapType::Readback, a_name);
		}
	}

	void ImagePrograms::Release(ImageBuffers& a_buffers) const
	{
		_gpu.Release(a_buffers.scratch);
		_gpu.Release(a_buffers.blocks);
		a_buffers.upload = {};
		a_buffers.readback = {};
	}

	void ImagePrograms::Stage(const ImageBuffers& a_buffers, const PlacedImage& a_image, const texture::MipChain& a_chain)
	{
		TW_ZONE("GPU image stage");
		auto offset = a_image.uploadOffset;
		for (std::uint32_t level = 0; level < a_image.Levels(); ++level) {
			const auto& source = a_chain.levels[level];
			std::memcpy(a_buffers.upload.mapped + offset, source.rgba.data(), source.rgba.size());
			offset += source.rgba.size();
		}
	}

	void ImagePrograms::RecordUploads(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const
	{
		bool any = false;
		for (const auto* image : a_images) {
			if (!image->uploaded) continue;
			a_commands.CopyBufferRegion(a_buffers.scratch.resource->GetHandle(), image->levelOffsets.front(), a_buffers.upload.resource->GetHandle(),
				image->uploadOffset, image->ScratchBytes());
			any = true;
		}
		if (any) {
			ContextImpl::Barrier(a_commands, rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest, rhi::ResourceSyncState::ComputeShading,
				rhi::ResourceAccessType::UnorderedAccess);
		}
	}

	void ImagePrograms::RecordMips(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const
	{
		// A level at a time for every image, each waiting for the one above.
		for (std::uint32_t level = 1;; ++level) {
			bool any = false;
			for (const auto* image : a_images) {
				if (image->uploaded || level >= image->Levels()) continue;
				any = true;
				const layout::HalfSizeConstants constants{ .buffer = a_buffers.scratch.uav,
					.sourceOffset = static_cast<std::uint32_t>(image->levelOffsets[level - 1]),
					.destinationOffset = static_cast<std::uint32_t>(image->levelOffsets[level]),
					.sourceWidth = image->LevelWidth(level - 1),
					.sourceHeight = image->LevelHeight(level - 1),
					.destinationWidth = image->LevelWidth(level),
					.destinationHeight = image->LevelHeight(level),
					.pad = 0 };
				ContextImpl::Dispatch(a_commands, _halfSize, &constants, (constants.destinationWidth + 7) / 8, (constants.destinationHeight + 7) / 8, 1);
			}
			if (!any) break;
			ContextImpl::Barrier(a_commands, rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
				rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
		}
	}

	void ImagePrograms::RecordBlocks(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const
	{
		for (const auto* image : a_images) {
			if (!image->GpuBlocks()) continue;
			for (std::uint32_t level = 0; level < image->Levels(); ++level) {
				const auto width = image->LevelWidth(level);
				const auto height = image->LevelHeight(level);
				const auto blocksWide = (width + 3) / 4;
				const auto blocksHigh = (height + 3) / 4;
				const layout::Bc7Constants constants{ .source = a_buffers.scratch.uav,
					.sourceOffset = static_cast<std::uint32_t>(image->levelOffsets[level]),
					.destination = a_buffers.blocks.uav,
					.destinationOffset = static_cast<std::uint32_t>(image->blockOffsets[level]),
					.width = width,
					.height = height,
					.rowPitch = blocksWide * 16,
					.pad = 0 };
				ContextImpl::Dispatch(a_commands, _bc7, &constants, (blocksWide + 7) / 8, (blocksHigh + 7) / 8, 1);
			}
		}
	}

	void ImagePrograms::RecordReadback(rhi::CommandList& a_commands, const ImageBuffers& a_buffers, std::span<const PlacedImage* const> a_images) const
	{
		ContextImpl::Barrier(a_commands, rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceSyncState::Copy,
			rhi::ResourceAccessType::CopySource);
		for (const auto* image : a_images) {
			const auto& source = image->GpuBlocks() ? a_buffers.blocks : a_buffers.scratch;
			const auto offset = image->GpuBlocks() ? image->blockOffsets.front() : image->levelOffsets.front();
			a_commands.CopyBufferRegion(a_buffers.readback.resource->GetHandle(), image->readbackOffset, source.resource->GetHandle(), offset, image->readbackBytes);
		}
	}

	std::vector<std::uint8_t> ImagePrograms::Read(const ImageBuffers& a_buffers, const PlacedImage& a_image)
	{
		const auto* from = reinterpret_cast<const std::uint8_t*>(a_buffers.readback.mapped) + a_image.readbackOffset;
		return std::vector<std::uint8_t>(from, from + a_image.readbackBytes);
	}

	texture::EncodeResult Wrap(const PlacedImage& a_image, std::span<const std::uint8_t> a_bytes)
	{
		TW_ZONE("GPU image DDS");
		texture::EncodeResult result;
		std::string error;
		std::optional<std::vector<std::uint8_t>> dds;
		if (a_image.GpuBlocks()) {
			dds = texture::EncodeDdsBlocks(a_image.width, a_image.height, a_image.Levels(), a_image.format, a_bytes, &error);
		} else {
			texture::MipChain chain;
			std::size_t offset = 0;
			for (std::uint32_t level = 0; level < a_image.Levels(); ++level) {
				texture::Image image(a_image.LevelWidth(level), a_image.LevelHeight(level));
				std::memcpy(image.rgba.data(), a_bytes.data() + offset, image.rgba.size());
				offset += image.rgba.size();
				chain.levels.push_back(std::move(image));
			}
			dds = texture::EncodeDds(chain, a_image.format, &error);
		}
		if (dds) {
			result.dds = std::move(*dds);
		} else {
			result.error = error.empty() ? std::string("encoding failed") : error;
		}
		return result;
	}
}