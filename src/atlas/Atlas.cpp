#include "tesworld/Atlas.h"
#include "tesworld/Profiling.h"

#include <array>
#include <tuple>
#include <numeric>
#include <bit>
#include <limits>
#include <memory>

#include <oneapi/tbb/parallel_for.h>

#include <xatlas.h>

namespace tesworld::atlas
{
	namespace
	{
		struct AtlasDeleter
		{
			void operator()(xatlas::Atlas* a_atlas) const { xatlas::Destroy(a_atlas); }
		};
		using AtlasPtr = std::unique_ptr<xatlas::Atlas, AtlasDeleter>;
	}

	namespace
	{
		struct Generated
		{
			AtlasPtr atlas;
			std::string error;
		};

		// xatlas over these meshes, each scaled by its density: xatlas has one texel density for all meshes.
		Generated Generate(std::span<const ChartMesh> a_meshes, const AtlasOptions& a_options, std::uint32_t a_resolution)
		{
			Generated generated{ AtlasPtr(xatlas::Create()), {} };
			std::vector<std::vector<Vec3>> scaled(a_meshes.size());
			for (std::size_t i = 0; i < a_meshes.size(); ++i) {
				const auto& mesh = a_meshes[i];
				scaled[i].reserve(mesh.positions.size());
				for (const auto& p : mesh.positions) scaled[i].push_back(p * mesh.density);
				xatlas::MeshDecl decl{};
				decl.vertexCount = static_cast<std::uint32_t>(scaled[i].size());
				decl.vertexPositionData = scaled[i].data();
				decl.vertexPositionStride = sizeof(Vec3);
				if (mesh.normals.size() == mesh.positions.size() && !mesh.normals.empty()) {
					decl.vertexNormalData = mesh.normals.data();
					decl.vertexNormalStride = sizeof(Vec3);
				}
				decl.indexData = mesh.indices.data();
				decl.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
				decl.indexFormat = xatlas::IndexFormat::UInt32;
				decl.faceCount = decl.indexCount / 3;
				if (mesh.faceMaterials.size() == decl.faceCount && decl.faceCount > 0) {
					decl.faceMaterialData = mesh.faceMaterials.data();
				}
				decl.epsilon = 1.0e-6f;
				const auto result = xatlas::AddMesh(generated.atlas.get(), decl, static_cast<std::uint32_t>(a_meshes.size()));
				if (result != xatlas::AddMeshError::Success) {
					generated.error = "xatlas couldn't add mesh " + std::to_string(i) + ": " + xatlas::StringForEnum(result);
					return generated;
				}
			}

			xatlas::ChartOptions chartOptions{};
			xatlas::PackOptions packOptions{};
			packOptions.bilinear = true;
			packOptions.blockAlign = a_options.blockAlign;
			packOptions.bruteForce = a_options.bruteForce;
			packOptions.padding = a_options.padding;
			packOptions.resolution = a_resolution;
			packOptions.texelsPerUnit = a_options.texelsPerUnit;
			packOptions.maxChartSize = a_options.maxChartSize;
			// Generate's two halves, each its own zone.
			{
				TW_ZONE("xatlas charts");
				xatlas::ComputeCharts(generated.atlas.get(), chartOptions);
			}
			{
				TW_ZONE("xatlas pack");
				xatlas::PackCharts(generated.atlas.get(), packOptions);
			}
			if (generated.atlas->meshCount != a_meshes.size() || generated.atlas->width == 0 || generated.atlas->height == 0) {
				generated.error = "xatlas produced no atlas";
			}
			return generated;
		}

		// One of xatlas's meshes in an Atlas: its UVs moved by a_offset texels and its pages by a_firstPage, then divided
		// by the page size.
		OutMesh Collect(const xatlas::Mesh& a_source, Vec2 a_offset, std::uint32_t a_firstPage, std::uint32_t a_width, std::uint32_t a_height)
		{
			OutMesh mesh;
			mesh.indices.assign(a_source.indexArray, a_source.indexArray + a_source.indexCount);
			mesh.vertices.reserve(a_source.vertexCount);
			const float inverseWidth = 1.0f / static_cast<float>(a_width);
			const float inverseHeight = 1.0f / static_cast<float>(a_height);
			for (std::uint32_t v = 0; v < a_source.vertexCount; ++v) {
				const auto& vertex = a_source.vertexArray[v];
				mesh.vertices.push_back(OutVertex{
					.source = vertex.xref,
					.page = a_firstPage + (vertex.atlasIndex < 0 ? 0u : static_cast<std::uint32_t>(vertex.atlasIndex)),
					.uv = { (vertex.uv[0] + a_offset.x) * inverseWidth, (vertex.uv[1] + a_offset.y) * inverseHeight },
				});
			}
			return mesh;
		}

		std::optional<Atlas> PackPerMesh(std::span<const ChartMesh> a_meshes, const AtlasOptions& a_options, std::string* a_error)
		{
			std::vector<MeshAlone> alone(a_meshes.size());
			oneapi::tbb::parallel_for(std::size_t{ 0 }, a_meshes.size(), [&](std::size_t m) { alone[m] = PackAlone(a_meshes[m], a_options); });
			return PlaceAlone(a_meshes, std::move(alone), a_options, a_error);
		}
	}

	MeshAlone PackAlone(const ChartMesh& a_mesh, const AtlasOptions& a_options, std::uint32_t a_resolution)
	{
		TW_ZONE("atlas mesh");
		if (a_options.texelsPerUnit <= 0.0f) return MeshAlone{ .error = "packing each mesh by itself needs a texel density" };
		auto generated = Generate(std::span(&a_mesh, 1), a_options, a_resolution);
		if (!generated.error.empty()) return MeshAlone{ .error = std::move(generated.error) };
		const auto& atlas = *generated.atlas;
		// Its UVs in texels, and xatlas's atlas freed, here.
		return MeshAlone{ .mesh = Collect(atlas.meshes[0], {}, 0, 1, 1), .width = atlas.width, .height = atlas.height, .atlasCount = atlas.atlasCount };
	}

	std::optional<Atlas> PlaceAlone(std::span<const ChartMesh> a_meshes, std::vector<MeshAlone> a_alone, const AtlasOptions& a_options, std::string* a_error)
	{
		const auto fail = [&](std::string a_message) -> std::optional<Atlas> {
			if (a_error) *a_error = std::move(a_message);
			return std::nullopt;
		};
		auto& alone = a_alone;
		for (const auto& packed : alone) {
			if (!packed.error.empty()) return fail(packed.error);
		}
		// 2. Their rectangles placed on the pages. A mesh larger than a page is packed again at the page size, onto
		// pages of its own after the rest.
		const auto limit = a_options.resolution == 0 ? (std::numeric_limits<std::uint32_t>::max)() : a_options.resolution - 2 * a_options.padding;
		std::vector<Rect> rects;
		std::vector<std::size_t> placed, large;
		for (std::size_t m = 0; m < alone.size(); ++m) {
			const auto& packed = alone[m];
			if (packed.atlasCount > 1 || packed.width > limit || packed.height > limit) {
				large.push_back(m);
			} else {
				placed.push_back(m);
				rects.push_back({ packed.width, packed.height });
			}
		}
		std::string error;
		std::optional<RectPack> pack;
		{
			TW_ZONE("atlas rects");
			pack = PackRects(rects, a_options.resolution, a_options.padding, &error);
		}
		if (!pack) return fail(error);
		std::vector<MeshAlone> repacked(large.size());
		oneapi::tbb::parallel_for(std::size_t{ 0 }, large.size(), [&](std::size_t i) { repacked[i] = PackAlone(a_meshes[large[i]], a_options, a_options.resolution); });
		for (const auto& packed : repacked) {
			if (!packed.error.empty()) return fail(packed.error);
		}

		// 3. Each mesh's vertices moved to its place, and its UVs over the page.
		Atlas result;
		result.texelsPerUnit = a_options.texelsPerUnit;
		result.width = large.empty() ? pack->width : a_options.resolution;
		result.height = large.empty() ? pack->height : a_options.resolution;
		result.pageCount = placed.empty() ? 0 : pack->pageCount;
		result.meshes.resize(a_meshes.size());
		std::vector<std::uint32_t> largePages(large.size());
		for (std::size_t i = 0; i < large.size(); ++i) {
			largePages[i] = result.pageCount;
			result.pageCount += (std::max)(repacked[i].atlasCount, 1u);
		}
		const float inverseWidth = 1.0f / static_cast<float>(result.width), inverseHeight = 1.0f / static_cast<float>(result.height);
		const auto place = [&](MeshAlone& a_packed, Vec2 a_offset, std::uint32_t a_firstPage) {
			auto mesh = std::move(a_packed.mesh);
			for (auto& vertex : mesh.vertices) {
				vertex.page += a_firstPage;
				vertex.uv = { (vertex.uv.x + a_offset.x) * inverseWidth, (vertex.uv.y + a_offset.y) * inverseHeight };
			}
			return mesh;
		};
		oneapi::tbb::parallel_for(std::size_t{ 0 }, placed.size() + large.size(), [&](std::size_t i) {
			if (i < placed.size()) {
				const auto& at = pack->placements[i];
				result.meshes[placed[i]] = place(alone[placed[i]], { static_cast<float>(at.x), static_cast<float>(at.y) }, at.page);
			} else {
				const auto j = i - placed.size();
				result.meshes[large[j]] = place(repacked[j], {}, largePages[j]);
			}
		});
		result.pageCount = (std::max)(result.pageCount, 1u);
		return result;
	}

	std::optional<Atlas> BuildAtlas(std::span<const ChartMesh> a_meshes, const AtlasOptions& a_options, std::string* a_error)
	{
		if (a_options.packing == Packing::PerMesh) {
			return PackPerMesh(a_meshes, a_options, a_error);
		}
		auto generated = Generate(a_meshes, a_options, a_options.resolution);
		if (!generated.error.empty()) {
			if (a_error) *a_error = std::move(generated.error);
			return std::nullopt;
		}
		const auto& atlas = *generated.atlas;
		Atlas result;
		result.width = atlas.width;
		result.height = atlas.height;
		result.pageCount = (std::max)(atlas.atlasCount, 1u);
		result.texelsPerUnit = atlas.texelsPerUnit;
		result.meshes.reserve(a_meshes.size());
		for (std::size_t i = 0; i < a_meshes.size(); ++i) result.meshes.push_back(Collect(atlas.meshes[i], {}, 0, atlas.width, atlas.height));
		return result;
	}

	namespace
	{
		// A page's skyline: the top of what's placed, as runs of equal height from left to right.
		class Skyline
		{
		public:
			Skyline(std::uint32_t a_width, std::uint32_t a_height) :
				_width(a_width),
				_height(a_height),
				_runs{ { 0, 0, a_width } }
			{}

			// Where a_width x a_height goes lowest (then leftmost), if anywhere.
			[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> Find(std::uint32_t a_width, std::uint32_t a_height) const
			{
				std::optional<std::pair<std::uint32_t, std::uint32_t>> best;
				for (std::size_t i = 0; i < _runs.size(); ++i) {
					const auto x = _runs[i].x;
					if (x + a_width > _width) break;
					std::uint32_t y = 0, covered = 0;
					for (std::size_t j = i; j < _runs.size() && covered < a_width; ++j) {
						y = (std::max)(y, _runs[j].y);
						covered = _runs[j].x + _runs[j].width - x;
					}
					if (y + a_height > _height) continue;
					if (!best || y < best->second) best = std::pair{ x, y };
				}
				return best;
			}

			void Place(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_width, std::uint32_t a_height)
			{
				const auto right = a_x + a_width;
				// Each run's parts left and right of what's placed, and what's placed between them.
				std::vector<Run> runs;
				runs.reserve(_runs.size() + 2);
				bool inserted = false;
				for (const auto& run : _runs) {
					const auto end = run.x + run.width;
					if (run.x < a_x) runs.push_back({ run.x, run.y, (std::min)(end, a_x) - run.x });
					if (!inserted && end > a_x) {
						runs.push_back({ a_x, a_y + a_height, a_width });
						inserted = true;
					}
					if (end > right) {
						const auto from = (std::max)(run.x, right);
						runs.push_back({ from, run.y, end - from });
					}
				}
				// Neighbours of one height are one run.
				_runs.clear();
				for (const auto& run : runs) {
					if (!_runs.empty() && _runs.back().y == run.y && _runs.back().x + _runs.back().width == run.x) {
						_runs.back().width += run.width;
					} else {
						_runs.push_back(run);
					}
				}
				_used = (std::max)(_used, a_y + a_height);
				_usedWidth = (std::max)(_usedWidth, right);
			}

			[[nodiscard]] std::uint32_t UsedHeight() const { return _used; }
			[[nodiscard]] std::uint32_t UsedWidth() const { return _usedWidth; }

		private:
			struct Run
			{
				std::uint32_t x;
				std::uint32_t y;
				std::uint32_t width;
			};

			std::uint32_t _width;
			std::uint32_t _height;
			std::vector<Run> _runs;
			std::uint32_t _used{ 0 };
			std::uint32_t _usedWidth{ 0 };
		};

		constexpr std::uint32_t AlignUp(std::uint32_t a_value) { return (a_value + 3u) & ~3u; }

		// Rectangles placed on pages of a_width x a_height, tallest first; false if one can't fit any page.
		bool PackSkyline(std::span<const Rect> a_rects, std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_padding, RectPack& a_pack)
		{
			std::vector<std::size_t> order(a_rects.size());
			std::iota(order.begin(), order.end(), std::size_t{ 0 });
			std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
				return std::tie(a_rects[b].height, a_rects[b].width) < std::tie(a_rects[a].height, a_rects[a].width);
			});
			std::vector<Skyline> pages;
			a_pack.placements.assign(a_rects.size(), Placement{});
			std::uint32_t usedWidth = 0, usedHeight = 0;
			for (const auto i : order) {
				// Whole 4x4 blocks, the padding after each rectangle.
				const auto width = AlignUp(a_rects[i].width + a_padding);
				const auto height = AlignUp(a_rects[i].height + a_padding);
				if (width > a_width || height > a_height) return false;
				bool placed = false;
				for (std::uint32_t page = 0; page < pages.size() && !placed; ++page) {
					if (const auto at = pages[page].Find(width, height)) {
						pages[page].Place(at->first, at->second, width, height);
						a_pack.placements[i] = { page, at->first, at->second };
						placed = true;
					}
				}
				if (!placed) {
					pages.emplace_back(a_width, a_height);
					const auto at = pages.back().Find(width, height);
					pages.back().Place(at->first, at->second, width, height);
					a_pack.placements[i] = { static_cast<std::uint32_t>(pages.size() - 1), at->first, at->second };
				}
			}
			for (const auto& page : pages) {
				usedWidth = (std::max)(usedWidth, page.UsedWidth());
				usedHeight = (std::max)(usedHeight, page.UsedHeight());
			}
			a_pack.pageCount = (std::max)(static_cast<std::uint32_t>(pages.size()), 1u);
			a_pack.width = usedWidth;
			a_pack.height = usedHeight;
			return true;
		}
	}

	std::optional<RectPack> PackRects(std::span<const Rect> a_rects, std::uint32_t a_resolution, std::uint32_t a_padding, std::string* a_error)
	{
		TW_ZONE("PackRects");
		const auto fail = [&](std::string a_message) -> std::optional<RectPack> {
			if (a_error) *a_error = std::move(a_message);
			return std::nullopt;
		};
		if (a_rects.empty()) {
			return RectPack{};
		}
		RectPack pack;
		if (a_resolution != 0) {
			if (!PackSkyline(a_rects, a_resolution, a_resolution, a_padding, pack)) return fail("a rectangle is larger than a page");
			// Pages of the size asked for, once there's more than one; one page only as large as it's used.
			if (pack.pageCount > 1) pack.width = pack.height = a_resolution;
			return pack;
		}
		// One page as large as needed: of the widths a power of two apart, the one whose page, each side rounded up to a
		// power of two, is smallest.
		std::uint64_t area = 0;
		std::uint32_t widest = 0;
		for (const auto& rect : a_rects) {
			area += static_cast<std::uint64_t>(AlignUp(rect.width + a_padding)) * AlignUp(rect.height + a_padding);
			widest = (std::max)(widest, AlignUp(rect.width + a_padding));
		}
		std::optional<RectPack> best;
		std::uint64_t bestArea = 0;
		for (auto width = std::bit_ceil(widest); static_cast<std::uint64_t>(width) * width / 4 <= area * 4 || !best; width *= 2) {
			RectPack candidate;
			if (!PackSkyline(a_rects, width, (std::numeric_limits<std::uint32_t>::max)() / 2, a_padding, candidate)) continue;
			const auto candidateArea = static_cast<std::uint64_t>(std::bit_ceil((std::max)(candidate.width, 4u))) * std::bit_ceil((std::max)(candidate.height, 4u));
			if (!best || candidateArea < bestArea) {
				best = std::move(candidate);
				bestArea = candidateArea;
			}
			if (width >= (1u << 30)) break;
		}
		return best;
	}
}
