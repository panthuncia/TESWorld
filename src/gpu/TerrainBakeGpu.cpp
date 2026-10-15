#include "tesworld/TerrainBakeGpu.h"

#include "ClientQueue.h"
#include "ContextImpl.h"
#include "ImageFrames.h"
#include "Layout.h"

#include "tesworld/Profiling.h"

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_group.h>

#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <ranges>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace tesworld::texture
{
	namespace
	{
		using gpu::detail::ComputeProgram;
		using gpu::detail::ContextImpl;
		using gpu::detail::DeviceBuffer;
		using gpu::detail::ImageBuffers;
		using gpu::detail::ImageLayout;
		using gpu::detail::ImagePrograms;
		using gpu::detail::PlacedImage;
		namespace layout = gpu::layout;

		// A frame bakes requests until their images reach this many texels (16 of terrain's finest blocks, 512x512 twice).
		constexpr std::uint64_t kFrameTexels = 8ull << 20;
		constexpr std::size_t kFrameRequests = 64;
		constexpr std::size_t kFrameSets = 3;
		// What resident data may take of the adapter's own memory.
		constexpr double kMemoryShare = 0.7;

		std::uint32_t LevelCount(std::uint32_t a_size)
		{
			std::uint32_t levels = 1;
			while (a_size > 1) {
				a_size = (std::max)(1u, a_size / 2);
				++levels;
			}
			return levels;
		}

		std::uint32_t FloatBits(float a_value)
		{
			std::uint32_t bits;
			std::memcpy(&bits, &a_value, sizeof(bits));
			return bits;
		}

		// Words of a buffer the kernels read, with the struct layouts of Layout.hlsli.
		class Words
		{
		public:
			[[nodiscard]] std::uint32_t Offset() const { return static_cast<std::uint32_t>(_words.size() * 4); }
			void Align(std::size_t a_bytes)
			{
				while ((_words.size() * 4) % a_bytes != 0) _words.push_back(0);
			}
			template <class T>
			std::uint32_t Append(const T& a_value)
			{
				static_assert(sizeof(T) % 4 == 0);
				const auto offset = Offset();
				const auto start = _words.size();
				_words.resize(start + sizeof(T) / 4);
				std::memcpy(_words.data() + start, &a_value, sizeof(T));
				return offset;
			}
			std::uint32_t AppendWords(std::span<const std::uint32_t> a_words)
			{
				const auto offset = Offset();
				_words.insert(_words.end(), a_words.begin(), a_words.end());
				return offset;
			}
			template <class T>
			void Set(std::uint32_t a_offset, const T& a_value)
			{
				std::memcpy(reinterpret_cast<std::byte*>(_words.data()) + a_offset, &a_value, sizeof(T));
			}
			[[nodiscard]] std::span<const std::byte> Bytes() const { return std::as_bytes(std::span(_words)); }

		private:
			std::vector<std::uint32_t> _words;
		};

		struct Job
		{
			TerrainBakeRequest request;
			TerrainBaker::Callback done;
		};

		struct Placed
		{
			Job job;
			PlacedImage diffuse;
			PlacedImage normal;
		};

		struct FrameSet
		{
			ImageBuffers buffers;
			std::vector<Placed> placed;
			bool busy{ false };
		};

		struct GpuMaterial
		{
			DeviceBuffer buffer;
		};

		class GpuTerrainBaker final : public TerrainBaker, public gpu::detail::Client
		{
		public:
			GpuTerrainBaker(gpu::Context& a_context, const WorldTerrain& a_terrain, TerrainBakeSettings a_settings) :
				_context(a_context),
				_gpu(a_context.Impl()),
				_terrain(a_terrain),
				_settings(a_settings),
				_queue(_gpu, kFrameSets, [this](Job a_job) { BakeOnCpu(std::move(a_job)); })
			{
				TW_ZONE("GPU terrain baker");
				const auto world = BuildWorld();
				Reserve(world.Bytes().size());
				_gpu.Run([&] {
					_bake = _gpu.CreateProgram("TerrainBake", "BakeCS", sizeof(layout::BakeConstants) / 4);
					_images.emplace(_gpu);
					_world = _gpu.Upload(world.Bytes(), "tesworld terrain lattice");
				});
				_gpu.AddClient(*this);
			}

			~GpuTerrainBaker() noexcept override
			{
				Wait();
				_gpu.RemoveClient(*this);
				if (!_queue.Failed()) {
					try {
						_gpu.Run([&] {
							_gpu.CompleteAllInFlight();
							for (const auto& frame : _queue.Frames()) _images->Release(frame->buffers);
							for (auto& material : _materials) _gpu.Release(material.buffer);
							for (auto& chunk : _pool) _gpu.Release(chunk);
							_gpu.Release(_world);
						});
					} catch (...) {
						// The context failed meanwhile: it releases everything itself.
					}
				}
			}

			std::uint32_t AddMaterial(const TerrainMaterial& a_material) override
			{
				TW_ZONE("GPU terrain material");
				_cpuMaterials.push_back(&a_material);
				auto [material, pool] = Flatten(a_material);
				Reserve(material.Bytes().size() + pool.Bytes().size());
				_gpu.Run([&] {
					if (!pool.Bytes().empty()) {
						auto chunk = _gpu.Upload(pool.Bytes(), "tesworld terrain textures");
						for (const auto index : _pendingPoolLevels) _poolLevels[index].pool = chunk.srv;
						_pool.push_back(std::move(chunk));
					}
					// The material's levels name their pool chunks' descriptors, known now.
					for (const auto& [offset, index] : _levelFixups) material.Set(offset, _poolLevels[index].pool);
					_materials.push_back({ _gpu.Upload(material.Bytes(), "tesworld terrain material") });
				});
				_pendingPoolLevels.clear();
				_levelFixups.clear();
				return static_cast<std::uint32_t>(_cpuMaterials.size() - 1);
			}

			void Bake(const TerrainBakeRequest& a_request, Callback a_done) override { _queue.Push({ a_request, std::move(a_done) }); }

			void Wait() override
			{
				TW_ZONE("GPU terrain bake wait");
				_queue.Wait();
			}

			[[nodiscard]] std::string Backend() const override { return "GPU, " + _context.Describe(); }

			[[nodiscard]] std::string Problem() const override
			{
				const auto failure = _queue.Failure();
				return failure.empty() ? std::string() : "the GPU failed (" + failure + "); the rest was baked on the CPU";
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
			void Reserve(std::size_t a_bytes)
			{
				_resident += a_bytes;
				const auto memory = _context.DedicatedMemory();
				if (memory != 0 && static_cast<double>(_resident) > static_cast<double>(memory) * kMemoryShare) {
					throw gpu::UnsupportedError(_context.Describe() + " has too little memory for this worldspace's terrain (" +
						std::to_string(_resident >> 20) + " MiB of " + std::to_string(memory >> 20) + ")");
				}
			}

			// The lattice the bake reads its heights and vertex colours from, a point beyond the land's cells on each side.
			Words BuildWorld() const
			{
				TW_ZONE("GPU terrain lattice");
				const auto minCell = _terrain.MinCell();
				const auto maxCell = _terrain.MaxCell();
				layout::WorldHeader header{};
				header.gx0 = minCell.x * kLandVerticesPerCell - 1;
				header.gy0 = minCell.y * kLandVerticesPerCell - 1;
				header.width = static_cast<std::uint32_t>((maxCell.x - minCell.x + 1) * kLandVerticesPerCell + 3);
				header.height = static_cast<std::uint32_t>((maxCell.y - minCell.y + 1) * kLandVerticesPerCell + 3);
				header.defaultHeight = _terrain.DefaultLandHeight();
				const auto points = static_cast<std::size_t>(header.width) * header.height;
				std::vector<std::uint32_t> heights(points), colours(points);
				oneapi::tbb::parallel_for(std::uint32_t{ 0 }, header.height, [&](std::uint32_t y) {
					for (std::uint32_t x = 0; x < header.width; ++x) {
						const auto gx = header.gx0 + static_cast<std::int32_t>(x);
						const auto gy = header.gy0 + static_cast<std::int32_t>(y);
						const auto index = static_cast<std::size_t>(y) * header.width + x;
						heights[index] = FloatBits(_terrain.HeightAt(gx, gy));
						// ColorAt is VCLR / 255: the byte comes back exactly.
						const auto colour = _terrain.ColorAt(gx, gy);
						const auto byte = [](float a_value) { return static_cast<std::uint32_t>(std::lround(a_value * 255.0f)); };
						colours[index] = byte(colour.x) | (byte(colour.y) << 8) | (byte(colour.z) << 16) | 0xFF000000u;
					}
				});
				Words words;
				words.Append(header);
				words.Align(16);
				header.heightsOffset = words.AppendWords(heights);
				header.colorsOffset = words.AppendWords(colours);
				words.Set(0, header);
				return words;
			}

			// A material's tables, and the textures of it not uploaded yet (a pool chunk of their own).
			std::pair<Words, Words> Flatten(const TerrainMaterial& a_material)
			{
				const auto& terrain = a_material.Terrain();
				const auto& settings = a_material.Settings();
				Words pool;
				std::vector<layout::TextureGpu> textures;
				std::vector<layout::LevelGpu> levels;
				std::vector<layout::StochasticGpu> stochastic;
				std::unordered_map<const void*, std::uint32_t> textureIndex, stochasticIndex;

				// An image's place in the pool: uploaded before, or appended to this material's chunk.
				const auto poolLevel = [&](const Image& a_image) {
					if (const auto it = _poolLevelIndex.find(&a_image); it != _poolLevelIndex.end()) return _poolLevels[it->second];
					pool.Align(16);
					const auto offset = pool.Offset();
					std::vector<std::uint32_t> words(a_image.rgba.size() / 4);
					std::memcpy(words.data(), a_image.rgba.data(), words.size() * 4);
					pool.AppendWords(words);
					const auto index = static_cast<std::uint32_t>(_poolLevels.size());
					_poolLevels.push_back({ .pool = layout::kNone, .offset = offset, .width = a_image.width, .height = a_image.height });
					_poolLevelIndex.emplace(&a_image, index);
					_pendingPoolLevels.push_back(index);
					return _poolLevels.back();
				};
				std::vector<std::pair<std::uint32_t, const Image*>> levelImages;
				const auto texture = [&](const MipChain* a_chain) -> std::uint32_t {
					if (!a_chain) return layout::kNone;
					if (const auto it = textureIndex.find(a_chain); it != textureIndex.end()) return it->second;
					const auto index = static_cast<std::uint32_t>(textures.size());
					textures.push_back({ .firstLevel = static_cast<std::uint32_t>(levels.size()), .levelCount = static_cast<std::uint32_t>(a_chain->levels.size()),
						.width = a_chain->Width(), .height = a_chain->Height() });
					for (const auto& level : a_chain->levels) {
						levelImages.push_back({ static_cast<std::uint32_t>(levels.size()), &level });
						levels.push_back(poolLevel(level));
					}
					textureIndex.emplace(a_chain, index);
					return index;
				};
				const auto stochasticTexture = [&](const StochasticTexture* a_texture) -> std::uint32_t {
					if (!a_texture) return layout::kNone;
					if (const auto it = stochasticIndex.find(a_texture); it != stochasticIndex.end()) return it->second;
					layout::StochasticGpu entry{};
					entry.gaussian = texture(&a_texture->gaussian);
					entry.lut = static_cast<std::uint32_t>(levels.size());
					levelImages.push_back({ entry.lut, &a_texture->inverseLut });
					levels.push_back(poolLevel(a_texture->inverseLut));
					entry.lutHeight = a_texture->lutHeight;
					entry.mode = a_texture->mode == StochasticMode::NormalXY ? layout::kStochasticNormalXY : layout::kStochasticDecorrelatedColor;
					entry.originX = a_texture->colorSpaceOrigin.x;
					entry.originY = a_texture->colorSpaceOrigin.y;
					entry.originZ = a_texture->colorSpaceOrigin.z;
					entry.v0X = a_texture->colorSpaceVector0.x;
					entry.v0Y = a_texture->colorSpaceVector0.y;
					entry.v0Z = a_texture->colorSpaceVector0.z;
					entry.v1X = a_texture->colorSpaceVector1.x;
					entry.v1Y = a_texture->colorSpaceVector1.y;
					entry.v1Z = a_texture->colorSpaceVector1.z;
					entry.v2X = a_texture->colorSpaceVector2.x;
					entry.v2Y = a_texture->colorSpaceVector2.y;
					entry.v2Z = a_texture->colorSpaceVector2.z;
					const auto index = static_cast<std::uint32_t>(stochastic.size());
					stochastic.push_back(entry);
					stochasticIndex.emplace(a_texture, index);
					return index;
				};

				const bool stochasticDiffuse = settings.stochastic && settings.stochasticDiffuse;
				const bool stochasticNormal = settings.stochastic && settings.stochasticNormal;
				const bool gaussian = settings.stochastic && settings.gaussian;
				const bool derivativeNormals = stochasticNormal && settings.derivativeNormals;
				std::vector<layout::LayerGpu> layers;
				// Only the stochastic textures the shader may read: a layer with one wants a stochastic context, so it keeps the
				// diffuse's; with derivative normals (the default) a normal map is read as it is, never its Gaussian form, and the
				// normal map itself already asks for the context.
				const bool readsStochastic = gaussian && (stochasticDiffuse || stochasticNormal);
				for (const auto& layer : a_material.Layers()) {
					layout::LayerGpu entry{};
					entry.diffuse = texture(layer.diffuse.get());
					entry.normal = texture(layer.normal.get());
					entry.diffuseStochastic = readsStochastic ? stochasticTexture(layer.diffuseStochastic.get()) : layout::kNone;
					entry.normalStochastic = readsStochastic && !derivativeNormals ? stochasticTexture(layer.normalStochastic.get()) : layout::kNone;
					entry.uvScale = layer.uvScale;
					entry.stochasticScale = layer.stochasticScale;
					entry.fallbackR = layer.fallbackColor.x;
					entry.fallbackG = layer.fallbackColor.y;
					entry.fallbackB = layer.fallbackColor.z;
					entry.fallbackA = layer.fallbackColor.w;
					layers.push_back(entry);
				}

				layout::MaterialHeader header{};
				header.flags = (stochasticDiffuse ? layout::kMaterialStochasticDiffuse : 0u) | (stochasticNormal ? layout::kMaterialStochasticNormal : 0u) |
				               (gaussian ? layout::kMaterialGaussian : 0u) | (derivativeNormals ? layout::kMaterialDerivativeNormals : 0u);
				header.blendCurve = settings.blendCurve;
				std::vector<std::uint32_t> grid;
				if (!terrain.regions.empty()) {
					auto [minX, maxX] = std::ranges::minmax(terrain.regions | std::views::transform([](const TerrainRegionDescriptor& r) { return r.regionX; }));
					auto [minY, maxY] = std::ranges::minmax(terrain.regions | std::views::transform([](const TerrainRegionDescriptor& r) { return r.regionY; }));
					header.rx0 = minX;
					header.ry0 = minY;
					header.gridWidth = static_cast<std::uint32_t>(maxX - minX + 1);
					header.gridHeight = static_cast<std::uint32_t>(maxY - minY + 1);
					grid.assign(static_cast<std::size_t>(header.gridWidth) * header.gridHeight, 0);
					for (std::size_t i = 0; i < terrain.regions.size(); ++i) {
						const auto& region = terrain.regions[i];
						grid[static_cast<std::size_t>(region.regionY - minY) * header.gridWidth + static_cast<std::size_t>(region.regionX - minX)] =
							static_cast<std::uint32_t>(i + 1);
					}
				}
				Words words;
				words.Append(header);
				header.gridOffset = words.AppendWords(grid);
				header.regionsOffset = words.Offset();
				for (const auto& region : terrain.regions) {
					words.Append(layout::RegionGpu{ region.layerRefStart, region.layerRefCount, region.weightBlockStart, region.weightSampleSide });
				}
				header.layerRefsOffset = words.Offset();
				for (const auto& ref : terrain.layerRefs) words.Append(ref.layerIndex);
				header.layerRefCount = static_cast<std::uint32_t>(terrain.layerRefs.size());
				header.weightsOffset = words.AppendWords(terrain.weightBlocks);
				header.weightCount = static_cast<std::uint32_t>(terrain.weightBlocks.size());
				header.layersOffset = words.Offset();
				for (const auto& layer : layers) words.Append(layer);
				header.layerCount = static_cast<std::uint32_t>(layers.size());
				header.texturesOffset = words.Offset();
				for (const auto& entry : textures) words.Append(entry);
				header.stochasticOffset = words.Offset();
				for (const auto& entry : stochastic) words.Append(entry);
				// Levels last: their pool descriptors are filled in once this material's chunk is uploaded (AddMaterial).
				header.levelsOffset = words.Offset();
				_levelFixups.clear();
				for (const auto& [index, image] : levelImages) {
					_levelFixups.push_back({ header.levelsOffset + index * 16 + static_cast<std::uint32_t>(offsetof(layout::LevelGpu, pool)), _poolLevelIndex.at(image) });
				}
				for (const auto& level : levels) words.Append(level);
				words.Set(0, header);
				return { std::move(words), std::move(pool) };
			}

			// Takes requests in order into the frame, and places their images.
			void TakeFrame(FrameSet& a_frame, std::deque<Job>& a_waiting)
			{
				ImageLayout layout;
				std::uint64_t texels = 0;
				while (!a_waiting.empty() && a_frame.placed.size() < kFrameRequests) {
					const auto& request = a_waiting.front().request;
					const auto jobTexels = static_cast<std::uint64_t>(request.diffuseSize) * request.diffuseSize + static_cast<std::uint64_t>(request.normalSize) * request.normalSize;
					if (!a_frame.placed.empty() && texels + jobTexels > kFrameTexels) break;
					texels += jobTexels;
					Placed placed{ .job = std::move(a_waiting.front()) };
					a_waiting.pop_front();
					const auto& taken = placed.job.request;
					placed.diffuse = layout.Place(taken.diffuseSize, taken.diffuseSize, LevelCount(taken.diffuseSize), taken.diffuseFormat, false);
					placed.normal = layout.Place(taken.normalSize, taken.normalSize, LevelCount(taken.normalSize), taken.normalFormat, false);
					a_frame.placed.push_back(std::move(placed));
				}
				_images->Ensure(a_frame.buffers, layout, "tesworld terrain images");
			}

			void Record(FrameSet& a_frame)
			{
				TW_ZONE("GPU terrain frame");
				a_frame.busy = true;
				const auto world = _world.srv;
				_gpu.ExecuteFrame(
					[&](rhi::CommandList& a_commands) {
						using Sync = rhi::ResourceSyncState;
						using Access = rhi::ResourceAccessType;
						const auto uavBarrier = [&] { ContextImpl::Barrier(a_commands, Sync::ComputeShading, Access::UnorderedAccess, Sync::ComputeShading, Access::UnorderedAccess); };
						for (const auto& placed : a_frame.placed) {
							const auto& request = placed.job.request;
							layout::BakeConstants constants{};
							constants.world = world;
							constants.material = _materials.at(request.material).buffer.srv;
							constants.output = a_frame.buffers.scratch.uav;
							constants.flags = _settings.vertexColors ? layout::kBakeVertexColors : 0u;
							constants.diffuseOffset = static_cast<std::uint32_t>(placed.diffuse.levelOffsets.front());
							constants.normalOffset = static_cast<std::uint32_t>(placed.normal.levelOffsets.front());
							constants.diffuseSize = request.diffuseSize;
							constants.normalSize = request.normalSize;
							constants.originX = request.origin.x;
							constants.originY = request.origin.y;
							constants.extent = request.extent;
							constants.gain = _settings.gain;
							constants.bias = _settings.bias;
							constants.fallbackR = _settings.fallbackAlbedo.x;
							constants.fallbackG = _settings.fallbackAlbedo.y;
							constants.fallbackB = _settings.fallbackAlbedo.z;
							const auto dispatch = [&](std::uint32_t a_flags, std::uint32_t a_size) {
								auto bake = constants;
								bake.flags |= a_flags;
								ContextImpl::Dispatch(a_commands, _bake, &bake, (a_size + 7) / 8, (a_size + 7) / 8, 1);
							};
							if (request.diffuseSize == request.normalSize) {
								dispatch(layout::kBakeDiffuse | layout::kBakeNormal, request.diffuseSize);
							} else {
								dispatch(layout::kBakeDiffuse, request.diffuseSize);
								dispatch(layout::kBakeNormal, request.normalSize);
							}
						}
						uavBarrier();
						std::vector<const PlacedImage*> images;
						for (const auto& placed : a_frame.placed) images.insert(images.end(), { &placed.diffuse, &placed.normal });
						_images->RecordMips(a_commands, a_frame.buffers, images);
						_images->RecordBlocks(a_commands, a_frame.buffers, images);
						_images->RecordReadback(a_commands, a_frame.buffers, images);
					},
					[this, &a_frame] { Complete(a_frame); });
			}

			// Render thread: the frame's bytes copied out (the readback is reused), each request finished in a task.
			void Complete(FrameSet& a_frame)
			{
				TW_ZONE("GPU terrain frame complete");
				for (auto& placed : a_frame.placed) {
					auto diffuseBytes = ImagePrograms::Read(a_frame.buffers, placed.diffuse);
					auto normalBytes = ImagePrograms::Read(a_frame.buffers, placed.normal);
					_queue.Run([this, job = std::move(placed.job), diffuse = std::move(placed.diffuse), normal = std::move(placed.normal),
									   diffuseBytes = std::move(diffuseBytes), normalBytes = std::move(normalBytes)] {
						TW_ZONE("GPU terrain DDS");
						TerrainBakeResult result;
						auto encodedDiffuse = gpu::detail::Wrap(diffuse, diffuseBytes);
						auto encodedNormal = gpu::detail::Wrap(normal, normalBytes);
						result.error = !encodedDiffuse.Ok() ? encodedDiffuse.error : encodedNormal.error;
						result.diffuseDds = std::move(encodedDiffuse.dds);
						result.normalDds = std::move(encodedNormal.dds);
						Finish(job, std::move(result));
					});
				}
				a_frame.placed.clear();
				a_frame.busy = false;
			}

			void Finish(const Job& a_job, TerrainBakeResult a_result)
			{
				a_job.done(std::move(a_result));
				_queue.Finished();
			}

			void BakeOnCpu(Job a_job)
			{
				{
					std::scoped_lock lock(_cpuMutex);
					if (!_cpu) {
						_cpu = CreateCpuTerrainBaker(_terrain, _settings);
						for (const auto* material : _cpuMaterials) _cpu->AddMaterial(*material);
					}
				}
				_cpu->Bake(a_job.request, [&](TerrainBakeResult a_result) { Finish(a_job, std::move(a_result)); });
			}

			gpu::Context& _context;
			ContextImpl& _gpu;
			const WorldTerrain& _terrain;
			TerrainBakeSettings _settings;
			std::size_t _resident{ 0 };
			std::vector<const TerrainMaterial*> _cpuMaterials;

			std::mutex _cpuMutex;
			std::unique_ptr<TerrainBaker> _cpu;

			// Built on the caller's thread, used on the render thread.
			std::vector<layout::LevelGpu> _poolLevels;
			std::unordered_map<const Image*, std::uint32_t> _poolLevelIndex;
			std::vector<std::uint32_t> _pendingPoolLevels;
			// Where the material being added names a pool chunk, and which pool level it is.
			std::vector<std::pair<std::uint32_t, std::uint32_t>> _levelFixups;

			// Render thread only.
			ComputeProgram _bake;
			std::optional<ImagePrograms> _images;
			DeviceBuffer _world;
			std::vector<GpuMaterial> _materials;
			std::vector<DeviceBuffer> _pool;

			// Last, so it's gone first: its tasks finish jobs with the rest.
			gpu::detail::ClientQueue<Job, FrameSet> _queue;
		};
	}

	std::unique_ptr<TerrainBaker> CreateGpuTerrainBaker(gpu::Context& a_context, const WorldTerrain& a_terrain, TerrainBakeSettings a_settings)
	{
		return std::make_unique<GpuTerrainBaker>(a_context, a_terrain, a_settings);
	}
}
