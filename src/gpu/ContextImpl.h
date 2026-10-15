#pragma once

// The GPU context's insides, for tesworld::gpu's own sources: BasicRHI and OpenRenderGraph stay out of public headers.
// The structure is FasterNGIO's GpuRejector (src/Gpu/GpuRejector.cpp) with its ray tracing taken out: a render thread
// owns the device and an OpenRenderGraph PersistentGraphHost; work reaches it as clients the thread pumps, each frame
// recorded by a callback and finished by another once the GPU has run it.

#include "tesworld/Gpu.h"

#include "MpscQueue.h"

#include <rhi.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace org
{
	class Buffer;
	class PersistentGraphHost;
}

namespace tesworld::gpu::detail
{
	std::span<const std::byte> EmbeddedShader(std::string_view a_name);

	struct MappedBuffer
	{
		rhi::ResourcePtr resource;
		std::byte* mapped{ nullptr };
		std::uint64_t bytes{ 0 };
	};

	// A device-local buffer with raw (ByteAddressBuffer) views.
	struct DeviceBuffer
	{
		rhi::ResourcePtr resource;
		std::uint64_t bytes{ 0 };
		std::uint32_t srv{ 0xFFFFFFFFu };
		std::uint32_t uav{ 0xFFFFFFFFu };
	};

	struct ComputeProgram
	{
		rhi::PipelineLayoutPtr layout;
		rhi::PipelinePtr pipeline;
		std::uint32_t constantWords{ 0 };
	};

	// A vertex and pixel shader drawing triangle lists into colour targets and a depth target, with push constants both
	// stages read. Vertices are pulled from buffers by the vertex shader: there's no input layout.
	struct GraphicsProgram
	{
		rhi::PipelineLayoutPtr layout;
		rhi::PipelinePtr pipeline;
		std::uint32_t constantWords{ 0 };
	};

	struct GraphicsProgramDesc
	{
		std::string_view shader;
		std::string_view vertexEntry;
		std::string_view pixelEntry;
		std::uint32_t constantWords{ 0 };
		std::span<const rhi::Format> colors;
		rhi::Format depth{ rhi::Format::Unknown };
	};

	// A device-local 2D texture drawn into (a render target, with a view shaders read, or a depth target).
	struct DeviceTexture
	{
		rhi::ResourcePtr resource;
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		rhi::Format format{ rhi::Format::Unknown };
		std::uint32_t srv{ 0xFFFFFFFFu };
		std::uint32_t rtv{ 0xFFFFFFFFu };
		std::uint32_t dsv{ 0xFFFFFFFFu };
	};

	// Work the render thread runs. Pump is called each time the thread wakes, until none has anything to do; it may
	// execute frames. Fail is called once if the thread fails: the client must finish what it owes.
	class Client
	{
	public:
		virtual ~Client() = default;
		virtual bool Pump() = 0;
		virtual void Fail(const std::string& a_reason) = 0;
	};

	struct PassContext;

	class ContextImpl
	{
	public:
		explicit ContextImpl(ContextDesc a_desc);
		~ContextImpl();

		// ---- Any thread ----
		// Runs a_task on the render thread and waits for it; its exception is rethrown here.
		void Run(const std::function<void()>& a_task);
		// Has the render thread pump its clients.
		void Wake();
		void AddClient(Client& a_client);
		void RemoveClient(Client& a_client);

		[[nodiscard]] Api ApiInUse() const { return _api; }
		[[nodiscard]] const std::string& Adapter() const { return _adapter; }
		[[nodiscard]] std::uint64_t DedicatedMemory() const { return _dedicatedMemory; }

		// ---- Render thread only ----
		[[nodiscard]] rhi::Device Device();
		[[nodiscard]] ComputeProgram CreateProgram(std::string_view a_shader, std::string_view a_entry, std::uint32_t a_constantWords);
		// Both entries in one shader, "<shader>.VS"/".PS" as the build names them (tesworld_gpu_shader's STAGE).
		[[nodiscard]] GraphicsProgram CreateGraphicsProgram(const GraphicsProgramDesc& a_desc);
		// A colour target (a_depth false: with an SRV) or a depth target.
		[[nodiscard]] DeviceTexture CreateTarget(std::uint32_t a_width, std::uint32_t a_height, rhi::Format a_format, bool a_depth, const char* a_name);
		void Release(DeviceTexture& a_texture);
		[[nodiscard]] rhi::DescriptorSlot RtvSlot(std::uint32_t a_index) const;
		[[nodiscard]] rhi::DescriptorSlot DsvSlot(std::uint32_t a_index) const;
		[[nodiscard]] MappedBuffer CreateMapped(std::uint64_t a_bytes, rhi::HeapType a_heap, const char* a_name);
		[[nodiscard]] DeviceBuffer CreateDeviceBuffer(std::uint64_t a_bytes, bool a_srv, bool a_uav, const char* a_name);
		void Release(DeviceBuffer& a_buffer);
		// A device buffer the kernels read (SRV), its bytes copied in by a frame of its own, waited for.
		[[nodiscard]] DeviceBuffer Upload(std::span<const std::byte> a_bytes, const char* a_name);
		// Records a frame and submits it; a_completed runs on this thread once the GPU has finished it.
		void ExecuteFrame(std::function<void(rhi::CommandList&)> a_record, std::function<void()> a_completed);
		// Every frame submitted is finished (and its completion run).
		void CompleteAllInFlight();

		static void Barrier(rhi::CommandList& a_commands, rhi::ResourceSyncState a_beforeSync, rhi::ResourceAccessType a_beforeAccess,
			rhi::ResourceSyncState a_afterSync, rhi::ResourceAccessType a_afterAccess);
		static void Dispatch(rhi::CommandList& a_commands, const ComputeProgram& a_program, const void* a_constants, std::uint32_t a_x,
			std::uint32_t a_y, std::uint32_t a_z);
		// A texture's layout changed, waiting on what came before (a_beforeSync, a_beforeAccess).
		static void Transition(rhi::CommandList& a_commands, const DeviceTexture& a_texture, rhi::ResourceSyncState a_beforeSync,
			rhi::ResourceAccessType a_beforeAccess, rhi::ResourceLayout a_beforeLayout, rhi::ResourceSyncState a_afterSync, rhi::ResourceAccessType a_afterAccess,
			rhi::ResourceLayout a_afterLayout);

	private:
		struct Task
		{
			std::function<void()> work;
			std::shared_ptr<std::promise<void>> done;
		};

		void Initialize();
		void CreateDevice(Api a_api);
		void Shutdown();
		void Loop();
		void OnFrameCompleted(std::uint64_t a_frame);
		std::uint32_t AllocateDescriptor();

		ContextDesc _desc;
		std::thread _thread;
		MpscQueue<Task> _inbox;
		std::atomic<std::uint64_t> _signal{ 0 };
		std::atomic<bool> _stopping{ false };
		std::atomic<bool> _failed{ false };
		std::string _failure;

		// Written by the render thread before the constructor returns.
		Api _api{ Api::D3D12 };
		std::string _adapter;
		std::uint64_t _dedicatedMemory{ 0 };

		// Render thread only.
		rhi::Backend _backend{ rhi::Backend::D3D12 };
		rhi::DevicePtr _device;
		rhi::DescriptorHeapPtr _heap;
		rhi::DescriptorHeapPtr _samplerHeap;
		std::uint32_t _nextDescriptor{ 0 };
		std::vector<std::uint32_t> _freeDescriptors;
		// Render and depth target views: a few, each a slot of its own heap.
		rhi::DescriptorHeapPtr _rtvHeap;
		rhi::DescriptorHeapPtr _dsvHeap;
		std::vector<std::uint32_t> _freeRtvs;
		std::vector<std::uint32_t> _freeDsvs;
		std::unique_ptr<org::PersistentGraphHost> _host;
		std::shared_ptr<org::Buffer> _heartbeat;
		std::shared_ptr<PassContext> _passContext;
		std::vector<Client*> _clients;
		std::map<std::uint64_t, std::function<void()>> _inFlight;
		std::uint64_t _frames{ 0 };
		std::uint64_t _failAfter{ 0 };
	};
}
