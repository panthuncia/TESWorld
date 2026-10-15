#pragma once

#include "tesworld/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Texture atlases with xatlas (at SARP's pin), as BasicRenderer's ObjectReyesAtlasBaker uses it: meshes cut into charts
// and packed into fixed-size pages, or plain rectangles packed. A bake then rasterizes each output triangle in its
// page's texel space.
namespace tesworld::atlas
{
	struct ChartMesh
	{
		std::span<const Vec3> positions;
		std::span<const Vec3> normals;
		std::span<const std::uint32_t> indices;
		// Triangles of different materials never share a chart (one per triangle; optional).
		std::span<const std::uint32_t> faceMaterials;
		// Texels per unit relative to AtlasOptions::texelsPerUnit, for meshes that want more or less detail.
		float density{ 1.0f };
	};

	enum class Packing
	{
		// Every mesh's charts packed together: the fullest pages, on one thread.
		Together,
		// Each mesh charted and packed by itself, on every core, and the meshes' rectangles then placed on the pages:
		// many times quicker for many meshes, the pages a little less full. Needs texelsPerUnit.
		PerMesh
	};

	struct AtlasOptions
	{
		// Each page's width and height (0: one page as large as needed).
		std::uint32_t resolution{ 4096 };
		std::uint32_t padding{ 4 };
		// 0 lets xatlas choose a density that fills one page.
		float texelsPerUnit{ 0.0f };
		std::uint32_t maxChartSize{ 0 };
		bool blockAlign{ true };
		bool bruteForce{ false };
		Packing packing{ Packing::Together };
	};

	struct OutVertex
	{
		std::uint32_t source{ 0 };
		std::uint32_t page{ 0 };
		// 0..1 across the page.
		Vec2 uv{};
	};

	struct OutMesh
	{
		std::vector<OutVertex> vertices;
		std::vector<std::uint32_t> indices;
	};

	struct Atlas
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t pageCount{ 0 };
		float texelsPerUnit{ 0.0f };
		std::vector<OutMesh> meshes;
	};

	[[nodiscard]] std::optional<Atlas> BuildAtlas(std::span<const ChartMesh> a_meshes, const AtlasOptions& a_options, std::string* a_error = nullptr);

	// Packing::PerMesh in two steps, for a caller that charts its meshes as its own work items: each mesh charted and
	// packed by itself (its UVs in texels of a page of its own), then the meshes placed on the pages. BuildAtlas is the
	// two together.
	struct MeshAlone
	{
		OutMesh mesh;
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t atlasCount{ 0 };
		std::string error;
	};

	// a_resolution 0: a page as large as the mesh needs.
	[[nodiscard]] MeshAlone PackAlone(const ChartMesh& a_mesh, const AtlasOptions& a_options, std::uint32_t a_resolution = 0);
	// a_alone[i] is a_meshes[i] packed alone; a mesh larger than a page is packed again at the page's size.
	[[nodiscard]] std::optional<Atlas> PlaceAlone(std::span<const ChartMesh> a_meshes, std::vector<MeshAlone> a_alone, const AtlasOptions& a_options,
		std::string* a_error = nullptr);

	struct Rect
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
	};

	struct Placement
	{
		std::uint32_t page{ 0 };
		std::uint32_t x{ 0 };
		std::uint32_t y{ 0 };
	};

	struct RectPack
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t pageCount{ 0 };
		std::vector<Placement> placements;
	};

	// Rectangles packed unrotated into pages of a_resolution (0: one page as large as needed), a_padding texels apart.
	[[nodiscard]] std::optional<RectPack> PackRects(std::span<const Rect> a_rects, std::uint32_t a_resolution, std::uint32_t a_padding, std::string* a_error = nullptr);

	// Calls a_texel(x, y, barycentric, inside) for every texel whose centre lies in the triangle (a, b, c in texel
	// units), and for texels up to a_expand texels outside its edges with the barycentric clamped onto the triangle
	// (inside false), so a bake fills the texels bilinear filtering reaches.
	template <class F>
	void RasterizeTriangle(Vec2 a, Vec2 b, Vec2 c, std::uint32_t a_width, std::uint32_t a_height, F&& a_texel, float a_expand = 0.0f)
	{
		const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
		if (std::abs(area) < 1.0e-12f) return;
		const auto minX = static_cast<std::int64_t>(std::floor((std::min)({ a.x, b.x, c.x }) - a_expand));
		const auto maxX = static_cast<std::int64_t>(std::ceil((std::max)({ a.x, b.x, c.x }) + a_expand));
		const auto minY = static_cast<std::int64_t>(std::floor((std::min)({ a.y, b.y, c.y }) - a_expand));
		const auto maxY = static_cast<std::int64_t>(std::ceil((std::max)({ a.y, b.y, c.y }) + a_expand));
		const float inverseArea = 1.0f / area;
		const auto edgeDistance = [](Vec2 p, Vec2 e0, Vec2 e1) {
			const Vec2 d{ e1.x - e0.x, e1.y - e0.y };
			const float length = std::sqrt(d.x * d.x + d.y * d.y);
			return length > 0.0f ? ((p.x - e0.x) * d.y - (p.y - e0.y) * d.x) / length : 0.0f;
		};
		for (auto y = (std::max<std::int64_t>)(minY, 0); y <= (std::min<std::int64_t>)(maxY, static_cast<std::int64_t>(a_height) - 1); ++y) {
			for (auto x = (std::max<std::int64_t>)(minX, 0); x <= (std::min<std::int64_t>)(maxX, static_cast<std::int64_t>(a_width) - 1); ++x) {
				const Vec2 p{ static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f };
				float w0 = ((b.x - p.x) * (c.y - p.y) - (c.x - p.x) * (b.y - p.y)) * inverseArea;
				float w1 = ((c.x - p.x) * (a.y - p.y) - (a.x - p.x) * (c.y - p.y)) * inverseArea;
				float w2 = 1.0f - w0 - w1;
				const bool inside = w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f;
				if (!inside) {
					if (a_expand <= 0.0f) continue;
					// Outside: near enough to an edge (whichever winding), then clamped onto the triangle.
					const float sign = area > 0.0f ? 1.0f : -1.0f;
					const float distance = (std::max)({ sign * edgeDistance(p, a, b), sign * edgeDistance(p, b, c), sign * edgeDistance(p, c, a) });
					if (distance > a_expand) continue;
					w0 = (std::max)(w0, 0.0f);
					w1 = (std::max)(w1, 0.0f);
					w2 = (std::max)(w2, 0.0f);
					const float sum = w0 + w1 + w2;
					if (sum <= 0.0f) continue;
					w0 /= sum;
					w1 /= sum;
					w2 /= sum;
				}
				a_texel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), Vec3{ w0, w1, w2 }, inside);
			}
		}
	}
}
