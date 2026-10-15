#include "tesworld/RenderGpu.h"

#include "ClientQueue.h"
#include "ContextImpl.h"
#include "Layout.h"

#include "tesworld/Profiling.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tesworld::render
{
	namespace
	{
		using gpu::detail::ContextImpl;
		using gpu::detail::DeviceBuffer;
		using gpu::detail::DeviceTexture;
		using gpu::detail::GraphicsProgram;
		using gpu::detail::MappedBuffer;
		namespace layout = gpu::layout;

		// Each frame's views are rectangles of targets this large, in samples: sixteen of a 256-pixel billboard's at four
		// samples a side. A view larger is rendered on the CPU.
		constexpr std::uint32_t kTarget = 4096;
		constexpr std::size_t kFrameSets = 2;
		// What a frame's geometry and textures may take; a view needing more goes alone.
		constexpr std::uint64_t kFrameBytes = 128ull << 20;
		// What the frames may take of the adapter's own memory.
		constexpr double kMemoryShare = 0.7;
		constexpr rhi::Format kSampleFormat = rhi::Format::R16G16B16A16_Float;
		constexpr rhi::Format kDepthFormat = rhi::Format::D32_Float;
		constexpr std::uint32_t kDrawWords = sizeof(layout::ViewDrawConstants) / 4;
		constexpr std::uint32_t kResolveWords = sizeof(layout::ViewResolveConstants) / 4;

		struct Job
		{
			ViewRequest request;
			ViewRenderer::Callback done;
		};

		struct Placed
		{
			Job job;
			// The view's rectangle of the targets, in samples, from its top left.
			std::uint32_t x{ 0 };
			std::uint32_t y{ 0 };
			std::uint32_t firstDraw{ 0 };
			std::uint32_t drawCount{ 0 };
			// Its image and normal map in the output and readback.
			std::uint64_t colorOffset{ 0 };
			std::uint64_t normalOffset{ 0 };
		};

		struct FrameSet
		{
			std::vector<Placed> placed;
			bool busy{ false };
			DeviceTexture color;
			DeviceTexture normal;
			DeviceTexture depth;
			// The frame's geometry, draws and textures (layout::ViewFrameHeader), uploaded then copied to the device.
			DeviceBuffer data;
			MappedBuffer upload;
			std::uint64_t dataBytes{ 0 };
			// Each view's image and normal map, resolved, then read back.
			DeviceBuffer output;
			MappedBuffer readback;
			std::uint64_t outputBytes{ 0 };
		};

		std::uint32_t Supersample(const RenderOptions& a_options) { return (std::max)(1u, a_options.supersample); }

		std::uint64_t Align(std::uint64_t a_value) { return (a_value + 15) / 16 * 16; }

		std::uint64_t ChainBytes(const texture::MipChain& a_chain)
		{
			std::uint64_t bytes = 0;
			for (const auto& level : a_chain.levels) bytes += static_cast<std::uint64_t>(level.width) * level.height * 4;
			return bytes;
		}

		// The texture a shader names, as the request holds it; null for none.
		const texture::MipChain* TextureOf(const ViewRequest& a_request, const std::string& a_path)
		{
			if (a_path.empty()) return nullptr;
			const auto it = a_request.textures.find(a_path);
			return it != a_request.textures.end() && it->second && !it->second->Empty() ? it->second.get() : nullptr;
		}

		// What a frame needs for a view beyond what it has: its vertices, indices, draws, and textures not yet in it.
		std::uint64_t BytesOf(const ViewRequest& a_request, const std::unordered_map<const texture::MipChain*, std::uint32_t>& a_textures)
		{
			std::uint64_t bytes = 0;
			std::vector<const texture::MipChain*> added;
			for (const auto& shape : a_request.model->shapes) {
				if (!Draws(shape, a_request.options.skinned)) continue;
				bytes += shape.positions.size() * layout::kViewVertexBytes + shape.indices.size() * 4 + sizeof(layout::ViewDrawGpu);
				for (const auto& path : { shape.shader.textures[0], shape.shader.textures[1] }) {
					const auto* chain = TextureOf(a_request, path);
					if (!chain || a_textures.contains(chain) || std::ranges::contains(added, chain)) continue;
					added.push_back(chain);
					bytes += ChainBytes(*chain) + chain->levels.size() * sizeof(layout::LevelGpu) + sizeof(layout::TextureGpu);
				}
			}
			return bytes;
		}

		class GpuViewRenderer final : public ViewRenderer, public gpu::detail::Client
		{
		public:
			explicit GpuViewRenderer(gpu::Context& a_context) :
				_context(a_context),
				_gpu(a_context.Impl()),
				_cpu(CreateCpuViewRenderer()),
				_queue(_gpu, kFrameSets, [this](Job a_job) { RenderOnCpu(std::move(a_job)); })
			{
				// Each frame set's targets, its data and its output, on the device.
				const auto targets = static_cast<double>(kTarget) * kTarget * (8 + 8 + 4);
				const auto frameBytes = targets + kFrameBytes + static_cast<double>(kTarget) * kTarget / 2;
				const auto memory = static_cast<double>(_context.DedicatedMemory());
				if (memory != 0 && frameBytes * kFrameSets > memory * kMemoryShare) {
					throw gpu::UnsupportedError(_context.Describe() + " has too little memory to render views");
				}
				_gpu.Run([&] {
					const rhi::Format colors[] = { kSampleFormat, kSampleFormat };
					_draw = _gpu.CreateGraphicsProgram(gpu::detail::GraphicsProgramDesc{
						.shader = "View", .vertexEntry = "ViewVS", .pixelEntry = "ViewPS", .constantWords = kDrawWords, .colors = colors, .depth = kDepthFormat });
					_resolve = _gpu.CreateProgram("ViewResolve", "ResolveCS", kResolveWords);
				});
				_gpu.AddClient(*this);
			}

			~GpuViewRenderer() noexcept override
			{
				Wait();
				_gpu.RemoveClient(*this);
				if (!_queue.Failed()) {
					try {
						_gpu.Run([&] {
							_gpu.CompleteAllInFlight();
							for (const auto& frame : _queue.Frames()) {
								for (auto* texture : { &frame->color, &frame->normal, &frame->depth }) {
									if (texture->resource) _gpu.Release(*texture);
								}
								if (frame->data.resource) _gpu.Release(frame->data);
								if (frame->output.resource) _gpu.Release(frame->output);
							}
						});
					} catch (...) {
						// The context failed meanwhile: it releases everything itself.
					}
				}
			}

			void Render(ViewRequest a_request, Callback a_done) override
			{
				const auto ss = Supersample(a_request.options);
				const bool fits = a_request.model && a_request.options.width != 0 && a_request.options.height != 0 &&
				                  static_cast<std::uint64_t>(a_request.options.width) * ss <= kTarget &&
				                  static_cast<std::uint64_t>(a_request.options.height) * ss <= kTarget;
				if (!fits) {
					_cpu->Render(std::move(a_request), std::move(a_done));
					return;
				}
				_queue.Push({ std::move(a_request), std::move(a_done) });
			}

			void Wait() override
			{
				TW_ZONE("GPU view render wait");
				_queue.Wait();
				_cpu->Wait();
			}

			[[nodiscard]] std::string Backend() const override { return "GPU, " + _context.Describe(); }

			[[nodiscard]] std::string Problem() const override
			{
				const auto failure = _queue.Failure();
				return failure.empty() ? std::string() : "the GPU failed (" + failure + "); the rest was rendered on the CPU";
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
			// Takes views in order while their rectangles fit the targets (in shelves, left to right) and their data the
			// frame's budget, then lays out and stages the frame's data.
			void TakeFrame(FrameSet& a_frame, std::deque<Job>& a_waiting)
			{
				TW_ZONE("GPU view frame take");
				std::unordered_map<const texture::MipChain*, std::uint32_t> textures;
				std::uint32_t cursorX = 0, cursorY = 0, shelf = 0;
				std::uint64_t bytes = sizeof(layout::ViewFrameHeader);
				while (!a_waiting.empty()) {
					const auto& request = a_waiting.front().request;
					const auto ss = Supersample(request.options);
					const auto wide = request.options.width * ss, high = request.options.height * ss;
					auto x = cursorX, y = cursorY;
					bool newShelf = false;
					if (x + wide > kTarget) {
						x = 0;
						y = cursorY + shelf;
						newShelf = true;
					}
					if (y + high > kTarget) break;
					const auto needs = BytesOf(request, textures);
					if (!a_frame.placed.empty() && bytes + needs > kFrameBytes) break;
					if (newShelf) {
						cursorY = y;
						shelf = 0;
					}
					cursorX = x + wide;
					shelf = (std::max)(shelf, high);
					bytes += needs;
					for (const auto& shape : request.model->shapes) {
						if (!Draws(shape, request.options.skinned)) continue;
						for (const auto& path : { shape.shader.textures[0], shape.shader.textures[1] }) {
							if (const auto* chain = TextureOf(request, path)) textures.try_emplace(chain, static_cast<std::uint32_t>(textures.size()));
						}
					}
					a_frame.placed.push_back(Placed{ .job = std::move(a_waiting.front()), .x = x, .y = y });
					a_waiting.pop_front();
				}
				Stage(a_frame);
			}

			// Grows a device buffer (and its host side) to hold a_bytes.
			void Ensure(DeviceBuffer& a_device, MappedBuffer& a_host, rhi::HeapType a_heap, std::uint64_t a_bytes, bool a_uav, const char* a_name)
			{
				if (a_device.resource && a_device.bytes >= a_bytes) return;
				if (a_device.resource) _gpu.Release(a_device);
				const auto bytes = (std::max)(a_bytes, a_device.bytes * 3 / 2);
				a_device = _gpu.CreateDeviceBuffer(bytes, !a_uav, a_uav, a_name);
				a_host = _gpu.CreateMapped(a_device.bytes, a_heap, a_name);
			}

			// The frame's geometry, draws and textures laid out in its data buffer (as View.hlsl reads them), and where its
			// views' images go in the output.
			void Stage(FrameSet& a_frame)
			{
				TW_ZONE("GPU view frame stage");
				std::vector<const texture::MipChain*> textures;
				std::unordered_map<const texture::MipChain*, std::uint32_t> textureIndex;
				std::uint64_t vertices = 0, indices = 0, draws = 0, levels = 0, texels = 0, output = 0;
				for (auto& placed : a_frame.placed) {
					const auto& request = placed.job.request;
					for (const auto& shape : request.model->shapes) {
						if (!Draws(shape, request.options.skinned)) continue;
						vertices += shape.positions.size();
						indices += shape.indices.size();
						++draws;
						for (const auto& path : { shape.shader.textures[0], shape.shader.textures[1] }) {
							const auto* chain = TextureOf(request, path);
							if (!chain || textureIndex.contains(chain)) continue;
							textureIndex.emplace(chain, static_cast<std::uint32_t>(textures.size()));
							textures.push_back(chain);
							levels += chain->levels.size();
							texels += ChainBytes(*chain);
						}
					}
					const auto pixels = static_cast<std::uint64_t>(request.options.width) * request.options.height * 4;
					placed.colorOffset = output;
					placed.normalOffset = output + pixels;
					output += pixels * 2;
				}
				layout::ViewFrameHeader header{};
				header.targetSize = kTarget;
				header.vertexOffset = static_cast<std::uint32_t>(Align(sizeof(header)));
				header.indexOffset = static_cast<std::uint32_t>(Align(header.vertexOffset + vertices * layout::kViewVertexBytes));
				header.drawOffset = static_cast<std::uint32_t>(Align(header.indexOffset + indices * 4));
				header.textureOffset = static_cast<std::uint32_t>(Align(header.drawOffset + draws * sizeof(layout::ViewDrawGpu)));
				header.levelOffset = static_cast<std::uint32_t>(Align(header.textureOffset + textures.size() * sizeof(layout::TextureGpu)));
				const auto texelOffset = Align(header.levelOffset + levels * sizeof(layout::LevelGpu));
				a_frame.dataBytes = texelOffset + texels;
				a_frame.outputBytes = (std::max<std::uint64_t>)(output, 4);
				Ensure(a_frame.data, a_frame.upload, rhi::HeapType::Upload, a_frame.dataBytes, false, "tesworld view frame");
				Ensure(a_frame.output, a_frame.readback, rhi::HeapType::Readback, a_frame.outputBytes, true, "tesworld view output");
				if (!a_frame.color.resource) {
					a_frame.color = _gpu.CreateTarget(kTarget, kTarget, kSampleFormat, false, "tesworld view colour");
					a_frame.normal = _gpu.CreateTarget(kTarget, kTarget, kSampleFormat, false, "tesworld view normal");
					a_frame.depth = _gpu.CreateTarget(kTarget, kTarget, kDepthFormat, true, "tesworld view depth");
				}

				auto* data = a_frame.upload.mapped;
				std::memcpy(data, &header, sizeof(header));
				auto* vertexOut = reinterpret_cast<float*>(data + header.vertexOffset);
				auto* indexOut = reinterpret_cast<std::uint32_t*>(data + header.indexOffset);
				auto* drawOut = reinterpret_cast<layout::ViewDrawGpu*>(data + header.drawOffset);
				std::uint32_t vertexCount = 0, indexCount = 0, drawCount = 0;
				for (auto& placed : a_frame.placed) {
					const auto& request = placed.job.request;
					const auto& camera = request.camera;
					const auto& options = request.options;
					const auto ss = Supersample(options);
					// The view's depth range: what's drawn, a little beyond.
					float nearest = std::numeric_limits<float>::max(), farthest = -std::numeric_limits<float>::max();
					for (const auto& shape : request.model->shapes) {
						if (!Draws(shape, options.skinned)) continue;
						for (const auto& position : shape.positions) {
							const auto depth = Dot(position - camera.center, camera.forward);
							nearest = (std::min)(nearest, depth);
							farthest = (std::max)(farthest, depth);
						}
					}
					const float margin = (std::max)(1.0f, (farthest - nearest) * 0.01f);
					placed.firstDraw = drawCount;
					for (const auto& shape : request.model->shapes) {
						if (!Draws(shape, options.skinned)) continue;
						const auto& shader = shape.shader;
						const auto count = shape.positions.size();
						const bool hasUv = shape.uvs.size() == count;
						const bool hasNormals = shape.normals.size() == count;
						const bool hasTangents = hasNormals && shape.tangents.size() == count && shape.bitangents.size() == count;
						const bool vertexColors = shader.VertexColors() && shape.colors.size() == count;
						for (std::size_t v = 0; v < count; ++v) {
							const auto& p = shape.positions[v];
							const auto n = hasNormals ? shape.normals[v] : Vec3{};
							const auto t = hasTangents ? shape.tangents[v] : Vec3{};
							const auto b = hasTangents ? shape.bitangents[v] : Vec3{};
							const auto uv = hasUv ? shape.uvs[v] : Vec2{};
							const auto c = vertexColors ? shape.colors[v] : Vec4{ 1, 1, 1, 1 };
							const float vertex[] = { p.x, p.y, p.z, n.x, n.y, n.z, t.x, t.y, t.z, b.x, b.y, b.z, uv.x, uv.y, c.x, c.y, c.z, c.w };
							std::memcpy(vertexOut, vertex, sizeof(vertex));
							vertexOut += std::size(vertex);
						}
						// Triangles naming a vertex the shape lacks are left out, as the CPU leaves them.
						const auto firstIndex = indexCount;
						for (std::size_t i = 0; i + 2 < shape.indices.size(); i += 3) {
							const auto i0 = shape.indices[i], i1 = shape.indices[i + 1], i2 = shape.indices[i + 2];
							if (i0 >= count || i1 >= count || i2 >= count) continue;
							indexOut[indexCount++] = i0;
							indexOut[indexCount++] = i1;
							indexOut[indexCount++] = i2;
						}
						const bool cutout = shader.alphaTest || shader.alphaBlend;
						std::uint32_t flags = 0;
						if (hasUv) flags |= layout::kViewHasUv;
						if (hasNormals) flags |= layout::kViewHasNormals;
						if (hasTangents) flags |= layout::kViewHasTangents;
						if (vertexColors) flags |= layout::kViewVertexColors;
						// A tree's vertex alpha is how much the wind moves it, not how opaque it is; grass's too.
						if (shader.VertexAlpha() && !shader.TreeAnim() && !options.windAlpha) flags |= layout::kViewVertexAlpha;
						if (cutout) flags |= layout::kViewCutout;
						if (shader.ModelSpaceNormals()) flags |= layout::kViewModelSpace;
						const auto* diffuse = TextureOf(request, shader.textures[0]);
						const auto* normalMap = TextureOf(request, shader.textures[1]);
						drawOut[drawCount++] = layout::ViewDrawGpu{
							.firstIndex = firstIndex,
							.triangleCount = (indexCount - firstIndex) / 3,
							.vertexBase = vertexCount,
							.flags = flags,
							.centerX = camera.center.x,
							.centerY = camera.center.y,
							.centerZ = camera.center.z,
							.width = camera.width,
							.rightX = camera.right.x,
							.rightY = camera.right.y,
							.rightZ = camera.right.z,
							.height = camera.height,
							.upX = camera.up.x,
							.upY = camera.up.y,
							.upZ = camera.up.z,
							.depthNear = nearest - margin,
							.forwardX = camera.forward.x,
							.forwardY = camera.forward.y,
							.forwardZ = camera.forward.z,
							.depthFar = farthest + margin,
							.diffuse = diffuse ? textureIndex.at(diffuse) : layout::kNone,
							.normalMap = normalMap ? textureIndex.at(normalMap) : layout::kNone,
							.samplesWide = static_cast<float>(options.width * ss),
							.samplesHigh = static_cast<float>(options.height * ss),
							.alpha = shader.alpha,
							.threshold = shader.alphaTest ? static_cast<float>(shader.alphaThreshold) / 255.0f : 0.5f,
							.uvScaleX = shader.uvScale.x,
							.uvScaleY = shader.uvScale.y,
							.uvOffsetX = shader.uvOffset.x,
							.uvOffsetY = shader.uvOffset.y,
							.direct = options.lighting.direct,
							.ambient = options.lighting.ambient,
							.lightX = options.lighting.toLight.x,
							.lightY = options.lighting.toLight.y,
							.lightZ = options.lighting.toLight.z,
							.pad = 0.0f,
						};
						vertexCount += static_cast<std::uint32_t>(count);
					}
					placed.drawCount = drawCount - placed.firstDraw;
				}

				auto* textureOut = reinterpret_cast<layout::TextureGpu*>(data + header.textureOffset);
				auto* levelOut = reinterpret_cast<layout::LevelGpu*>(data + header.levelOffset);
				std::uint64_t texelAt = texelOffset;
				std::uint32_t level = 0;
				for (std::size_t t = 0; t < textures.size(); ++t) {
					const auto& chain = *textures[t];
					textureOut[t] = layout::TextureGpu{ .firstLevel = level, .levelCount = static_cast<std::uint32_t>(chain.levels.size()), .width = chain.Width(),
						.height = chain.Height() };
					for (const auto& image : chain.levels) {
						levelOut[level++] = layout::LevelGpu{ .pool = a_frame.data.srv, .offset = static_cast<std::uint32_t>(texelAt), .width = image.width,
							.height = image.height };
						std::memcpy(data + texelAt, image.rgba.data(), image.rgba.size());
						texelAt += image.rgba.size();
					}
				}
			}

			void Record(FrameSet& a_frame)
			{
				TW_ZONE("GPU view frame");
				a_frame.busy = true;
				_gpu.ExecuteFrame(
					[&](rhi::CommandList& a_commands) {
						a_commands.CopyBufferRegion(a_frame.data.resource->GetHandle(), 0, a_frame.upload.resource->GetHandle(), 0, a_frame.dataBytes);
						ContextImpl::Barrier(a_commands, rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest, rhi::ResourceSyncState::AllShading,
							rhi::ResourceAccessType::ShaderResource);
						for (const auto* target : { &a_frame.color, &a_frame.normal }) {
							ContextImpl::Transition(a_commands, *target, rhi::ResourceSyncState::None, rhi::ResourceAccessType::None, rhi::ResourceLayout::Undefined,
								rhi::ResourceSyncState::RenderTarget, rhi::ResourceAccessType::RenderTarget, rhi::ResourceLayout::RenderTarget);
						}
						ContextImpl::Transition(a_commands, a_frame.depth, rhi::ResourceSyncState::None, rhi::ResourceAccessType::None, rhi::ResourceLayout::Undefined,
							rhi::ResourceSyncState::DepthStencil, rhi::ResourceAccessType::DepthReadWrite, rhi::ResourceLayout::DepthReadWrite);

						// Cleared, uncovered (alpha 0) and far; then each view drawn in its rectangle, under one viewport over the
						// whole target (View.hlsl).
						std::array<rhi::ColorAttachment, 2> colors{};
						std::size_t c = 0;
						for (const auto* target : { &a_frame.color, &a_frame.normal }) {
							auto& color = colors[c++];
							color.rtv = _gpu.RtvSlot(target->rtv);
							color.loadOp = rhi::LoadOp::Clear;
							color.storeOp = rhi::StoreOp::Store;
							color.resource = target->resource->GetHandle();
							color.clear.format = kSampleFormat;
							color.clear.rgba[0] = color.clear.rgba[1] = color.clear.rgba[2] = color.clear.rgba[3] = 0.0f;
						}
						rhi::DepthAttachment depth{};
						depth.dsv = _gpu.DsvSlot(a_frame.depth.dsv);
						depth.depthLoad = rhi::LoadOp::Clear;
						depth.depthStore = rhi::StoreOp::Store;
						depth.resource = a_frame.depth.resource->GetHandle();
						depth.clear.type = rhi::ClearValueType::DepthStencil;
						depth.clear.format = kDepthFormat;
						depth.clear.depthStencil.depth = 1.0f;
						a_commands.BeginPass(rhi::PassBeginInfo{ .colors = { colors.data(), static_cast<std::uint32_t>(colors.size()) }, .depth = &depth,
							.width = kTarget, .height = kTarget, .debugName = "tesworld views" });
						a_commands.BindLayout(_draw.layout->GetHandle());
						a_commands.BindPipeline(_draw.pipeline->GetHandle());
						a_commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
						const auto* draws = reinterpret_cast<const layout::ViewDrawGpu*>(a_frame.upload.mapped +
							reinterpret_cast<const layout::ViewFrameHeader*>(a_frame.upload.mapped)->drawOffset);
						for (const auto& placed : a_frame.placed) {
							for (std::uint32_t d = placed.firstDraw; d < placed.firstDraw + placed.drawCount; ++d) {
								if (draws[d].triangleCount == 0) continue;
								const layout::ViewDrawConstants constants{ .frame = a_frame.data.srv, .draw = d, .x = placed.x, .y = placed.y };
								a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kDrawWords, reinterpret_cast<const std::uint32_t*>(&constants));
								a_commands.Draw(draws[d].triangleCount * 3, 1, 0, 0);
							}
						}
						a_commands.EndPass();

						for (const auto* target : { &a_frame.color, &a_frame.normal }) {
							ContextImpl::Transition(a_commands, *target, rhi::ResourceSyncState::RenderTarget, rhi::ResourceAccessType::RenderTarget,
								rhi::ResourceLayout::RenderTarget, rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::ShaderResource,
								rhi::ResourceLayout::ShaderResource);
						}
						for (const auto& placed : a_frame.placed) {
							const auto& request = placed.job.request;
							const auto& camera = request.camera;
							const layout::ViewResolveConstants constants{
								.color = a_frame.color.srv,
								.normal = a_frame.normal.srv,
								.output = a_frame.output.uav,
								.supersample = Supersample(request.options),
								.x = placed.x,
								.y = placed.y,
								.width = request.options.width,
								.height = request.options.height,
								.colorOffset = static_cast<std::uint32_t>(placed.colorOffset),
								.normalOffset = static_cast<std::uint32_t>(placed.normalOffset),
								.pad0 = 0,
								.pad1 = 0,
								.rightX = camera.right.x,
								.rightY = camera.right.y,
								.rightZ = camera.right.z,
								.pad2 = 0.0f,
								.upX = camera.up.x,
								.upY = camera.up.y,
								.upZ = camera.up.z,
								.pad3 = 0.0f,
								.forwardX = camera.forward.x,
								.forwardY = camera.forward.y,
								.forwardZ = camera.forward.z,
								.pad4 = 0.0f,
							};
							ContextImpl::Dispatch(a_commands, _resolve, &constants, (request.options.width + 7) / 8, (request.options.height + 7) / 8, 1);
						}
						ContextImpl::Barrier(a_commands, rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceSyncState::Copy,
							rhi::ResourceAccessType::CopySource);
						a_commands.CopyBufferRegion(a_frame.readback.resource->GetHandle(), 0, a_frame.output.resource->GetHandle(), 0, a_frame.outputBytes);
					},
					[this, &a_frame] { Complete(a_frame); });
			}

			// Render thread: each view's image and normal map copied out of the readback, and its request finished in a task.
			void Complete(FrameSet& a_frame)
			{
				TW_ZONE("GPU view frame complete");
				for (auto& placed : a_frame.placed) {
					const auto& options = placed.job.request.options;
					ViewResult result;
					result.frame.color = texture::Image(options.width, options.height);
					result.frame.normal = texture::Image(options.width, options.height);
					std::memcpy(result.frame.color.rgba.data(), a_frame.readback.mapped + placed.colorOffset, result.frame.color.rgba.size());
					std::memcpy(result.frame.normal.rgba.data(), a_frame.readback.mapped + placed.normalOffset, result.frame.normal.rgba.size());
					// The request was only kept for the CPU, had the GPU failed.
					auto shared = std::make_shared<ViewResult>(std::move(result));
					_queue.Run([this, done = std::move(placed.job.done), shared] {
						done(std::move(*shared));
						_queue.Finished();
					});
				}
				a_frame.placed.clear();
				a_frame.busy = false;
			}

			void RenderOnCpu(Job a_job)
			{
				auto done = std::move(a_job.done);
				_cpu->Render(std::move(a_job.request), [this, done = std::move(done)](ViewResult a_result) {
					done(std::move(a_result));
					_queue.Finished();
				});
			}

			gpu::Context& _context;
			ContextImpl& _gpu;
			std::unique_ptr<ViewRenderer> _cpu;

			// Render thread only.
			GraphicsProgram _draw;
			gpu::detail::ComputeProgram _resolve;

			// Last, so it's gone first: its tasks finish jobs with the rest.
			gpu::detail::ClientQueue<Job, FrameSet> _queue;
		};
	}

	std::unique_ptr<ViewRenderer> CreateGpuViewRenderer(gpu::Context& a_context)
	{
		return std::make_unique<GpuViewRenderer>(a_context);
	}
}