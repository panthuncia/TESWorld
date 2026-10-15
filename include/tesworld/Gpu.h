#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// Compute on the GPU (tesworld::gpu, TESWORLD_WITH_GPU): a headless device, D3D12 or Vulkan through BasicRHI, run by an
// OpenRenderGraph host on a thread of its own, as FasterNGIO's ray-traced rejection is. Work that uses it (the terrain
// bake, TerrainBakeGpu.h) posts to that thread and is called back as frames complete. A machine without a suitable GPU
// throws UnsupportedError on creation, so a caller falls back to the CPU before any work is posted.
namespace tesworld::gpu
{
	enum class Api
	{
		D3D12,
		Vulkan
	};

	[[nodiscard]] const char* ApiName(Api a_api);

	// No GPU tesworld can use: the message says what was missing.
	class UnsupportedError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	struct ContextDesc
	{
		// One API, or (unset) D3D12 then Vulkan on Windows and Vulkan elsewhere. TESWORLD_GPU_API=d3d12|vulkan
		// overrides it.
		std::optional<Api> api;
		bool debugLayer{ false };
		// BasicRHI and OpenRenderGraph log through spdlog's default logger, to standard output. A tool whose standard output
		// is a protocol (oculory-lod --serve) wants it on standard error, warnings and worse only: this replaces the
		// default logger so. A host with its own logging leaves it false.
		bool quietLogging{ true };
	};

	namespace detail
	{
		class ContextImpl;
	}

	class Context
	{
	public:
		// Throws UnsupportedError when no adapter has what tesworld needs: shader model 6.6 with bindless descriptor
		// heaps (D3D12 resource binding tier 3 and enhanced barriers, or Vulkan's VK_EXT_descriptor_heap), not a
		// software adapter.
		explicit Context(ContextDesc a_desc = {});
		~Context();
		Context(const Context&) = delete;
		Context& operator=(const Context&) = delete;

		[[nodiscard]] Api ApiInUse() const;
		[[nodiscard]] const std::string& Adapter() const;
		[[nodiscard]] std::uint64_t DedicatedMemory() const;
		// "<adapter> (<API>)".
		[[nodiscard]] std::string Describe() const;

		// Runs a kernel and reads it back: true when the device does what it's asked.
		[[nodiscard]] bool SelfTest();

		[[nodiscard]] detail::ContextImpl& Impl() { return *_impl; }

	private:
		std::unique_ptr<detail::ContextImpl> _impl;
	};
}
