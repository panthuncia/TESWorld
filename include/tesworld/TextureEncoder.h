#pragma once

#include "tesworld/Texture.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Mip chains encoded to DDS out of the caller's way: on the CPU across its cores, or on the GPU
// (TextureEncoderGpu.h) when there is one. The chain is the caller's, mips and all, so what's encoded is the same
// whichever encodes it.
namespace tesworld::texture
{
	struct EncodeRequest
	{
		MipChain chain;
		Format format{ Format::BC7 };
		// Once set, a request not yet encoded calls back with the error "cancelled" (kCancelledError): the CPU stops at its
		// next band of block rows, the GPU before the request's frame is dispatched (one already dispatched completes).
		EncodeOptions options{};
	};

	struct EncodeResult
	{
		std::vector<std::uint8_t> dds;
		std::string error;

		[[nodiscard]] bool Ok() const { return error.empty(); }
	};

	class TextureEncoder
	{
	public:
		using Callback = std::function<void(EncodeResult)>;

		virtual ~TextureEncoder() = default;

		// Encodes, then calls a_done from a TBB task: an encoder may batch requests and finish them in any order.
		virtual void Encode(EncodeRequest a_request, Callback a_done) = 0;
		// Returns once every request so far has called back.
		virtual void Wait() = 0;
		// "CPU", or the GPU's API and adapter.
		[[nodiscard]] virtual std::string Backend() const = 0;
		// What went wrong that the encoder worked around (a GPU that failed part way), or nothing.
		[[nodiscard]] virtual std::string Problem() const { return {}; }
	};

	// EncodeDds in a task of its own for each request.
	[[nodiscard]] std::unique_ptr<TextureEncoder> CreateCpuTextureEncoder();
}