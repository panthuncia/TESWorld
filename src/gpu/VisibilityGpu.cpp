#include "tesworld/VisibilityGpu.h"

#include "ClientQueue.h"
#include "ContextImpl.h"
#include "Layout.h"

#include "tesworld/Profiling.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tesworld::gpu
{
	namespace
	{
		using detail::ContextImpl;
		using detail::DeviceBuffer;
		using detail::MappedBuffer;

		constexpr std::size_t kFrameSets = 2;
		// Groups in a frame: a launch per target and group, and a word each read back.
		constexpr std::uint32_t kFrameGroups = 256;
		constexpr std::uint64_t kStructureAlignment = 256;

		void Check(rhi::Result a_result, const char* a_what)
		{
			if (!rhi::IsOk(a_result)) {
				throw std::runtime_error(std::string("GPU: ") + a_what + " failed (rhi::Result " + std::to_string(static_cast<std::uint32_t>(a_result)) + ")");
			}
		}

		std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment)
		{
			return a_alignment == 0 ? a_value : (a_value + a_alignment - 1) / a_alignment * a_alignment;
		}

		template <class T>
		std::span<const std::byte> Bytes(const std::vector<T>& a_values)
		{
			return std::as_bytes(std::span<const T>(a_values));
		}

		// A trace's groups as frames take them: a part of at most kFrameGroups, joined back into the caller's results.
		struct TraceState
		{
			std::vector<VisibilityResult> results;
			std::atomic<std::size_t> parts{ 0 };
			VisibilityTracer::Callback done;
		};

		struct Job
		{
			std::shared_ptr<TraceState> trace;
			std::size_t first{ 0 };
			std::vector<VisibilityGroup> groups;
		};

		struct Placed
		{
			Job job;
			std::uint32_t firstGroup{ 0 };
		};

		struct FrameSet
		{
			// The groups' ranges, then the viewpoints, in host memory the rays read.
			MappedBuffer input;
			std::uint32_t inputView{ layout::kNone };
			DeviceBuffer output;
			MappedBuffer readback;
			std::uint32_t groups{ 0 };
			std::uint64_t viewpointsOffset{ 0 };
			std::vector<Placed> placed;
			bool busy{ false };
		};

		class GpuVisibilityTracer final : public VisibilityTracer, public detail::Client
		{
		public:
			GpuVisibilityTracer(Context& a_context, const VisibilityGround& a_ground, const VisibilityTargets& a_targets, float a_radius) :
				_context(a_context),
				_gpu(a_context.Impl()),
				_rows(a_targets.rows),
				_targets(a_targets.columns * a_targets.rows),
				_samples((std::max)(a_targets.samples, 1u)),
				_size(a_targets.size),
				_radius(a_radius),
				_queue(_gpu, kFrameSets, [this](Job a_job) { Unavailable(std::move(a_job), _queue.Failure()); })
			{
				const auto& support = _gpu.RayTracing();
				if (!support.supported) throw UnsupportedError(_context.Describe() + " can't trace rays: it lacks " + support.missing);
				if (a_ground.columns < 2 || a_ground.rows < 2 || a_ground.heights.size() != static_cast<std::size_t>(a_ground.columns) * a_ground.rows) {
					throw std::invalid_argument("visibility ground: heights must be columns x rows, at least 2 x 2");
				}
				if (_targets == 0 || a_targets.tops.size() != _targets) throw std::invalid_argument("visibility targets: tops must be columns x rows");
				if (support.maxWidth < _targets) throw UnsupportedError(_context.Describe() + " can't launch a ray per target");
				_frameGroups = (std::min)(kFrameGroups, support.maxHeight);
				if (_frameGroups == 0) throw UnsupportedError(_context.Describe() + " can't launch rays in two dimensions");
				_gpu.Run([&] { Build(a_ground, a_targets); });
				_gpu.AddClient(*this);
			}

			~GpuVisibilityTracer() noexcept override
			{
				Wait();
				_gpu.RemoveClient(*this);
				try {
					_gpu.Run([&] {
						_gpu.CompleteAllInFlight();
						for (const auto& frame : _queue.Frames()) ReleaseFrame(*frame);
						_gpu.ReleaseView(_sceneView);
						_gpu.Release(_vertices);
						_gpu.Release(_indices);
						_gpu.Release(_tops);
					});
				} catch (...) {
					// The context failed meanwhile: it releases everything itself.
				}
			}

			void Trace(std::vector<VisibilityGroup> a_groups, Callback a_done) override
			{
				auto trace = std::make_shared<TraceState>();
				trace->results.resize(a_groups.size());
				trace->done = std::move(a_done);
				const auto parts = a_groups.empty() ? 1 : (a_groups.size() + _frameGroups - 1) / _frameGroups;
				trace->parts = parts;
				for (std::size_t part = 0; part < parts; ++part) {
					const auto first = part * _frameGroups;
					const auto last = (std::min)(a_groups.size(), first + _frameGroups);
					Job job{ .trace = trace, .first = first };
					job.groups.assign(std::make_move_iterator(a_groups.begin() + static_cast<std::ptrdiff_t>(first)),
						std::make_move_iterator(a_groups.begin() + static_cast<std::ptrdiff_t>(last)));
					_queue.Push(std::move(job));
				}
			}

			void Wait() override
			{
				TW_ZONE("GPU visibility wait");
				_queue.Wait();
			}

			[[nodiscard]] std::string Problem() const override
			{
				const auto failure = _queue.Failure();
				return failure.empty() ? std::string() : "the GPU failed tracing visibility (" + failure + ")";
			}

			// ---- Render thread ----

			bool Pump() override
			{
				auto& waiting = _queue.Waiting();
				if (waiting.empty()) return false;
				auto* frame = _queue.AcquireFrame();
				if (!frame) return false;
				TakeFrame(*frame, waiting);
				Record(*frame);
				return true;
			}

			void Fail(const std::string& a_reason) override { _queue.Fail(a_reason); }

		private:
			// The ground's triangles, its acceleration structures, the targets' tops and the ray-tracing pipeline.
			void Build(const VisibilityGround& a_ground, const VisibilityTargets& a_targets)
			{
				TW_ZONE("GPU visibility build");
				auto device = _gpu.Device();
				const auto columns = a_ground.columns, rows = a_ground.rows;
				std::vector<float> vertices(static_cast<std::size_t>(columns) * rows * 3);
				for (std::uint32_t x = 0; x < columns; ++x) {
					for (std::uint32_t y = 0; y < rows; ++y) {
						auto* vertex = &vertices[(static_cast<std::size_t>(x) * rows + y) * 3];
						vertex[0] = static_cast<float>(x) * a_ground.spacing;
						vertex[1] = static_cast<float>(y) * a_ground.spacing;
						vertex[2] = a_ground.heights[static_cast<std::size_t>(x) * rows + y];
					}
				}
				std::vector<std::uint32_t> indices;
				indices.reserve(static_cast<std::size_t>(columns - 1) * (rows - 1) * 6);
				for (std::uint32_t x = 0; x + 1 < columns; ++x) {
					for (std::uint32_t y = 0; y + 1 < rows; ++y) {
						const auto southWest = x * rows + y, southEast = (x + 1) * rows + y;
						indices.insert(indices.end(), { southWest, southEast, southEast + 1, southWest, southEast + 1, southWest + 1 });
					}
				}
				_vertices = _gpu.Upload(Bytes(vertices), "tesworld visibility vertices");
				_indices = _gpu.Upload(Bytes(indices), "tesworld visibility indices");
				_tops = _gpu.Upload(Bytes(a_targets.tops), "tesworld visibility tops");

				rhi::RayTracingGeometryDesc ground{};
				ground.type = rhi::RayTracingGeometryType::Triangles;
				ground.flags = rhi::RTGeometry_Opaque;
				ground.triangles.vertexBuffer = { _vertices.resource->GetHandle(), 0, vertices.size() * sizeof(float) };
				ground.triangles.vertexStride = 3 * sizeof(float);
				ground.triangles.vertexFormat = rhi::Format::R32G32B32_Float;
				ground.triangles.vertexCount = columns * rows;
				ground.triangles.indexBuffer = { _indices.resource->GetHandle(), 0, indices.size() * sizeof(std::uint32_t) };
				ground.triangles.indexFormat = rhi::Format::R32_UInt;
				ground.triangles.indexCount = static_cast<std::uint32_t>(indices.size());
				rhi::AccelerationStructureBuildInputs bottom{};
				bottom.type = rhi::RayTracingAccelerationStructureType::BottomLevel;
				bottom.flags = rhi::RTASBuild_PreferFastTrace;
				bottom.geometries = { &ground, 1 };
				rhi::AccelerationStructurePrebuildInfo bottomSizes{};
				Check(device.GetAccelerationStructurePrebuildInfo(bottom, bottomSizes), "BLAS prebuild info");
				_blas = CreateStructure(rhi::RayTracingAccelerationStructureType::BottomLevel, bottomSizes, _blasStorage, "tesworld visibility BLAS");

				rhi::RayTracingInstanceDesc instance{};
				instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;
				instance.flags = rhi::RTInstance_ForceOpaque;
				const auto packed = rhi::PackRayTracingInstanceDesc(instance, device.GetAccelerationStructureDeviceAddress(_blas->GetHandle()));
				_instances = _gpu.CreateMapped(sizeof(packed), rhi::HeapType::Upload, "tesworld visibility instance");
				std::memcpy(_instances.mapped, &packed, sizeof(packed));
				rhi::RayTracingGeometryDesc instances{};
				instances.type = rhi::RayTracingGeometryType::Instances;
				instances.instances.instanceBuffer = { _instances.resource->GetHandle(), 0, sizeof(packed) };
				instances.instances.stride = sizeof(packed);
				instances.instances.count = 1;
				rhi::AccelerationStructureBuildInputs top{};
				top.type = rhi::RayTracingAccelerationStructureType::TopLevel;
				top.flags = rhi::RTASBuild_PreferFastTrace;
				top.geometries = { &instances, 1 };
				rhi::AccelerationStructurePrebuildInfo topSizes{};
				Check(device.GetAccelerationStructurePrebuildInfo(top, topSizes), "TLAS prebuild info");
				_tlas = CreateStructure(rhi::RayTracingAccelerationStructureType::TopLevel, topSizes, _tlasStorage, "tesworld visibility TLAS");

				// Both builds' scratch in one buffer, apart, so neither waits on the other's.
				const auto bottomScratch = AlignUp(bottomSizes.scratchDataSizeInBytes, kStructureAlignment);
				const auto topScratch = AlignUp(topSizes.scratchDataSizeInBytes, kStructureAlignment);
				rhi::ResourcePtr scratch;
				Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(bottomScratch + topScratch, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess,
							  "tesworld visibility scratch"),
						  scratch),
					"acceleration structure scratch");
				rhi::AccelerationStructureBuildDesc builds[2]{};
				builds[0].destination = _blas->GetHandle();
				builds[0].inputs = bottom;
				builds[0].scratch = { scratch->GetHandle(), 0, bottomScratch };
				builds[1].destination = _tlas->GetHandle();
				builds[1].inputs = top;
				builds[1].scratch = { scratch->GetHandle(), bottomScratch, topScratch };
				_gpu.ExecuteFrame(
					[&](rhi::CommandList& a_commands) {
						using Sync = rhi::ResourceSyncState;
						using Access = rhi::ResourceAccessType;
						a_commands.BuildAccelerationStructures(&builds[0], 1);
						ContextImpl::Barrier(a_commands, Sync::BuildRaytracingAccelerationStructure, Access::RaytracingAccelerationStructureWrite,
							Sync::BuildRaytracingAccelerationStructure, Access::RaytracingAccelerationStructureRead);
						a_commands.BuildAccelerationStructures(&builds[1], 1);
						ContextImpl::Barrier(a_commands, Sync::BuildRaytracingAccelerationStructure, Access::RaytracingAccelerationStructureWrite,
							Sync::Raytracing, Access::RaytracingAccelerationStructureRead);
					},
					[] {});
				// The scratch is freed once the builds are done.
				_gpu.CompleteAllInFlight();

				_sceneView = _gpu.AllocateView();
				rhi::SrvDesc scene{};
				scene.dimension = rhi::SrvDim::AccelerationStruct;
				scene.accel.accelerationStructure = _tlas->GetHandle();
				scene.accel.sizeBytes = AlignUp(topSizes.resultDataMaxSizeInBytes, kStructureAlignment);
				// The view addresses the TLAS itself; BasicRHI reads an empty resource as a null view.
				Check(device.CreateShaderResourceView(_gpu.ViewSlot(_sceneView), _tlasStorage->GetHandle(), scene), "TLAS view");

				CreatePipeline();
			}

			rhi::AccelerationStructurePtr CreateStructure(rhi::RayTracingAccelerationStructureType a_type, const rhi::AccelerationStructurePrebuildInfo& a_sizes,
				rhi::ResourcePtr& a_storage, const char* a_name)
			{
				auto device = _gpu.Device();
				const auto bytes = AlignUp(a_sizes.resultDataMaxSizeInBytes, kStructureAlignment);
				Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(bytes, rhi::HeapType::DeviceLocal,
							  static_cast<rhi::ResourceFlags>(rhi::RF_RaytracingAccelerationStructure | rhi::RF_AllowUnorderedAccess), a_name),
						  a_storage),
					a_name);
				rhi::AccelerationStructureDesc desc{};
				desc.type = a_type;
				desc.storage = a_storage->GetHandle();
				desc.sizeBytes = bytes;
				desc.flags = rhi::RTASBuild_PreferFastTrace;
				desc.debugName = a_name;
				rhi::AccelerationStructurePtr structure;
				Check(device.CreateAccelerationStructure(desc, structure), a_name);
				return structure;
			}

			void CreatePipeline()
			{
				auto device = _gpu.Device();
				const auto name = std::string("Visibility") + (_gpu.ApiInUse() == Api::D3D12 ? ".dxil" : ".spv");
				const auto bytes = detail::EmbeddedShader(name);
				if (bytes.empty()) throw UnsupportedError("this build has no " + name);
				rhi::PushConstantRangeDesc constants{};
				constants.visibility = rhi::ShaderStage::All;
				constants.num32BitValues = sizeof(layout::VisibilityConstants) / 4;
				Check(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PF_None }, _layout),
					"ray-tracing pipeline layout");
				const rhi::ShaderBinary binary{ bytes.data(), static_cast<std::uint32_t>(bytes.size()) };
				rhi::SubobjShader shaders[] = {
					{ rhi::ShaderStage::RayGen, binary, "VisibilityRayGen" },
					{ rhi::ShaderStage::Miss, binary, "VisibilityMiss" },
					{ rhi::ShaderStage::ClosestHit, binary, "VisibilityHit" },
				};
				rhi::RayTracingShaderGroupDesc groups[] = {
					{ .type = rhi::RayTracingShaderGroupType::General, .name = "VisibilityRayGen", .generalShader = "VisibilityRayGen" },
					{ .type = rhi::RayTracingShaderGroupType::General, .name = "VisibilityMiss", .generalShader = "VisibilityMiss" },
					{ .type = rhi::RayTracingShaderGroupType::TrianglesHitGroup, .name = "VisibilityHitGroup", .closestHitShader = "VisibilityHit" },
				};
				rhi::SubobjRayTracingPipeline pipeline{};
				pipeline.globalLayout = _layout->GetHandle();
				pipeline.shaders = { shaders, static_cast<std::uint32_t>(std::size(shaders)) };
				pipeline.shaderGroups = { groups, static_cast<std::uint32_t>(std::size(groups)) };
				pipeline.shaderConfig.maxPayloadSizeInBytes = 4;
				pipeline.shaderConfig.maxAttributeSizeInBytes = 8;
				pipeline.pipelineConfig.maxTraceRecursionDepth = 1;
				const rhi::PipelineStreamItem items[] = { rhi::Make(pipeline) };
				Check(device.CreatePipeline(items, 1, _pipeline), "ray-tracing pipeline");

				// The shader table: the ray generation, miss and hit group records, each region at the base alignment.
				const auto& support = _gpu.RayTracing();
				const auto handleSize = support.handleSize;
				std::vector<std::byte> handles(static_cast<std::size_t>(handleSize) * std::size(groups));
				Check(device.GetRayTracingShaderGroupHandles(_pipeline->GetHandle(), 0, static_cast<std::uint32_t>(std::size(groups)), handles.data(),
						  static_cast<std::uint32_t>(handles.size())),
					"shader group handles");
				_recordStride = rhi::AlignRayTracingShaderRecordSize(handleSize, support.strideAlignment);
				_missOffset = AlignUp(_recordStride, support.baseAlignment);
				_hitOffset = AlignUp(_missOffset + _recordStride, support.baseAlignment);
				_table = _gpu.CreateMapped(_hitOffset + _recordStride, rhi::HeapType::Upload, "tesworld visibility shader table");
				for (std::uint32_t group = 0; group < std::size(groups); ++group) {
					const auto offset = group == 0 ? 0 : group == 1 ? _missOffset : _hitOffset;
					rhi::WriteRayTracingShaderRecord(_table.mapped + offset, _recordStride, handles.data() + group * handleSize, handleSize);
				}
			}

			// A view of a host buffer's bytes, raw, for the rays to read.
			std::uint32_t RawView(const MappedBuffer& a_buffer)
			{
				const auto view = _gpu.AllocateView();
				rhi::SrvDesc srv{};
				srv.dimension = rhi::SrvDim::Buffer;
				srv.formatOverride = rhi::Format::R32_Typeless;
				srv.buffer.kind = rhi::BufferViewKind::Raw;
				srv.buffer.numElements = static_cast<std::uint32_t>(a_buffer.bytes / 4);
				Check(_gpu.Device().CreateShaderResourceView(_gpu.ViewSlot(view), a_buffer.resource->GetHandle(), srv), "raw SRV");
				return view;
			}

			void ReleaseFrame(FrameSet& a_frame)
			{
				_gpu.ReleaseView(a_frame.inputView);
				a_frame.inputView = layout::kNone;
				a_frame.input = {};
				_gpu.Release(a_frame.output);
				a_frame.readback = {};
			}

			// Takes jobs in order into the frame while their groups fit, and writes their ranges and viewpoints.
			void TakeFrame(FrameSet& a_frame, std::deque<Job>& a_waiting)
			{
				TW_ZONE("GPU visibility frame take");
				a_frame.groups = 0;
				std::uint64_t viewpoints = 0;
				while (!a_waiting.empty() && a_frame.groups + a_waiting.front().groups.size() <= _frameGroups) {
					Placed placed{ .job = std::move(a_waiting.front()), .firstGroup = a_frame.groups };
					a_waiting.pop_front();
					a_frame.groups += static_cast<std::uint32_t>(placed.job.groups.size());
					for (const auto& group : placed.job.groups) viewpoints += group.viewpoints.size();
					a_frame.placed.push_back(std::move(placed));
				}
				a_frame.viewpointsOffset = static_cast<std::uint64_t>(a_frame.groups) * 8;
				const auto inputBytes = a_frame.viewpointsOffset + viewpoints * 12;
				const auto outputBytes = static_cast<std::uint64_t>(a_frame.groups) * _targets * 4;
				if (a_frame.input.bytes < inputBytes) {
					_gpu.ReleaseView(a_frame.inputView);
					a_frame.input = _gpu.CreateMapped(AlignUp(inputBytes + inputBytes / 2, 256), rhi::HeapType::Upload, "tesworld visibility input");
					a_frame.inputView = RawView(a_frame.input);
				}
				if (a_frame.output.bytes < outputBytes) {
					_gpu.Release(a_frame.output);
					a_frame.output = _gpu.CreateDeviceBuffer(static_cast<std::uint64_t>(_frameGroups) * _targets * 4, false, true, "tesworld visibility output");
					a_frame.readback = _gpu.CreateMapped(a_frame.output.bytes, rhi::HeapType::Readback, "tesworld visibility readback");
				}
				auto* ranges = reinterpret_cast<std::uint32_t*>(a_frame.input.mapped);
				auto* points = reinterpret_cast<float*>(a_frame.input.mapped + a_frame.viewpointsOffset);
				std::uint32_t next = 0;
				for (const auto& placed : a_frame.placed) {
					for (const auto& group : placed.job.groups) {
						*ranges++ = next;
						*ranges++ = static_cast<std::uint32_t>(group.viewpoints.size());
						for (const auto& viewpoint : group.viewpoints) {
							*points++ = viewpoint[0];
							*points++ = viewpoint[1];
							*points++ = viewpoint[2];
						}
						next += static_cast<std::uint32_t>(group.viewpoints.size());
					}
				}
			}

			void Record(FrameSet& a_frame)
			{
				TW_ZONE("GPU visibility frame");
				a_frame.busy = true;
				_gpu.ExecuteFrame(
					[&](rhi::CommandList& a_commands) {
						const layout::VisibilityConstants constants{
							.scene = _sceneView,
							.input = a_frame.inputView,
							.tops = _tops.srv,
							.output = a_frame.output.uav,
							.rows = _rows,
							.targets = _targets,
							.samples = _samples,
							.viewpointsOffset = static_cast<std::uint32_t>(a_frame.viewpointsOffset),
							.size = _size,
							.radius = _radius,
							.pad0 = 0,
							.pad1 = 0,
						};
						a_commands.BindLayout(_layout->GetHandle());
						a_commands.BindPipeline(_pipeline->GetHandle());
						a_commands.PushConstants(rhi::ShaderStage::All, 0, 0, 0, sizeof(constants) / 4, reinterpret_cast<const std::uint32_t*>(&constants));
						rhi::RayTracingDispatchDesc dispatch{};
						const auto table = _table.resource->GetHandle();
						dispatch.rayGenerationShaderTable = { table, 0, _recordStride, _recordStride };
						dispatch.missShaderTable = { table, _missOffset, _recordStride, _recordStride };
						dispatch.hitGroupTable = { table, _hitOffset, _recordStride, _recordStride };
						dispatch.width = _targets;
						dispatch.height = a_frame.groups;
						a_commands.TraceRays(dispatch);
						ContextImpl::Barrier(a_commands, rhi::ResourceSyncState::Raytracing, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceSyncState::Copy,
							rhi::ResourceAccessType::CopySource);
						a_commands.CopyBufferRegion(a_frame.readback.resource->GetHandle(), 0, a_frame.output.resource->GetHandle(), 0,
							static_cast<std::uint64_t>(a_frame.groups) * _targets * 4);
					},
					[this, &a_frame] { Complete(a_frame); });
			}

			// Render thread: each job's flags copied out of the readback (reused), its trace called back once whole.
			void Complete(FrameSet& a_frame)
			{
				TW_ZONE("GPU visibility frame complete");
				const auto* words = reinterpret_cast<const std::uint32_t*>(a_frame.readback.mapped);
				for (auto& placed : a_frame.placed) {
					auto& trace = *placed.job.trace;
					for (std::size_t g = 0; g < placed.job.groups.size(); ++g) {
						const auto* row = words + (placed.firstGroup + g) * static_cast<std::size_t>(_targets);
						auto& visible = trace.results[placed.job.first + g].visible;
						visible.resize(_targets);
						for (std::uint32_t t = 0; t < _targets; ++t) visible[t] = row[t] != 0 ? 1 : 0;
					}
					Finish(std::move(placed.job.trace));
				}
				a_frame.placed.clear();
				a_frame.busy = false;
			}

			void Unavailable(Job a_job, const std::string& a_reason)
			{
				for (std::size_t g = 0; g < a_job.groups.size(); ++g) a_job.trace->results[a_job.first + g].error = a_reason.empty() ? "the GPU failed" : a_reason;
				Finish(std::move(a_job.trace));
			}

			// A part is done: the last calls the trace back, in a task.
			void Finish(std::shared_ptr<TraceState> a_trace)
			{
				if (a_trace->parts.fetch_sub(1, std::memory_order_acq_rel) != 1) {
					_queue.Finished();
					return;
				}
				_queue.Run([this, trace = std::move(a_trace)] {
					trace->done(std::move(trace->results));
					_queue.Finished();
				});
			}

			Context& _context;
			ContextImpl& _gpu;
			const std::uint32_t _rows;
			const std::uint32_t _targets;
			const std::uint32_t _samples;
			const float _size;
			const float _radius;
			std::uint32_t _frameGroups{ kFrameGroups };

			// Render thread only.
			DeviceBuffer _vertices;
			DeviceBuffer _indices;
			DeviceBuffer _tops;
			MappedBuffer _instances;
			rhi::ResourcePtr _blasStorage;
			rhi::ResourcePtr _tlasStorage;
			rhi::AccelerationStructurePtr _blas;
			rhi::AccelerationStructurePtr _tlas;
			std::uint32_t _sceneView{ layout::kNone };
			rhi::PipelineLayoutPtr _layout;
			rhi::PipelinePtr _pipeline;
			MappedBuffer _table;
			std::uint64_t _recordStride{ 0 };
			std::uint64_t _missOffset{ 0 };
			std::uint64_t _hitOffset{ 0 };

			// Last, so it's gone first: its tasks finish traces with the rest.
			detail::ClientQueue<Job, FrameSet> _queue;
		};
	}

	std::unique_ptr<VisibilityTracer> CreateVisibilityTracer(Context& a_context, const VisibilityGround& a_ground, const VisibilityTargets& a_targets, float a_radius)
	{
		return std::make_unique<GpuVisibilityTracer>(a_context, a_ground, a_targets, a_radius);
	}
}