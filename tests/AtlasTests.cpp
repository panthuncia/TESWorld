// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Atlas.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace tesworld;
using namespace tesworld::atlas;

namespace
{
	struct Box
	{
		std::uint32_t page;
		float minX, minY, maxX, maxY;
	};

	// Many meshes packed each by itself: every one inside its page, none over another, a mesh larger than a page on
	// pages of its own, and the same atlas every time.
	void PerMeshPackingPlacesMeshesApart(std::span<const Vec3> a_positions, std::span<const Vec3> a_normals, std::span<const std::uint32_t> a_indices)
	{
		std::vector<ChartMesh> meshes;
		for (int i = 0; i < 40; ++i) {
			meshes.push_back(ChartMesh{ .positions = a_positions, .normals = a_normals, .indices = a_indices, .density = 0.5f + 0.05f * static_cast<float>(i) });
		}
		// Its faces 640 texels a side: more than a page.
		meshes.push_back(ChartMesh{ .positions = a_positions, .normals = a_normals, .indices = a_indices, .density = 20.0f });
		const AtlasOptions options{ .resolution = 256, .padding = 2, .texelsPerUnit = 32.0f, .maxChartSize = 128, .packing = Packing::PerMesh };
		std::string error;
		const auto atlas = BuildAtlas(meshes, options, &error);
		assert(atlas && error.empty());
		assert(atlas->width == 256 && atlas->height == 256 && atlas->meshes.size() == meshes.size());

		std::vector<Box> boxes;
		for (const auto& mesh : atlas->meshes) {
			assert(mesh.indices.size() == a_indices.size());
			Box box{ mesh.vertices.front().page, 1, 1, 0, 0 };
			for (const auto& vertex : mesh.vertices) {
				assert(vertex.uv.x >= 0.0f && vertex.uv.x <= 1.0f && vertex.uv.y >= 0.0f && vertex.uv.y <= 1.0f);
				assert(vertex.page < atlas->pageCount);
				box.minX = (std::min)(box.minX, vertex.uv.x);
				box.minY = (std::min)(box.minY, vertex.uv.y);
				box.maxX = (std::max)(box.maxX, vertex.uv.x);
				box.maxY = (std::max)(box.maxY, vertex.uv.y);
			}
			// A triangle's corners on one page.
			for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
				const auto page = mesh.vertices[mesh.indices[i]].page;
				assert(mesh.vertices[mesh.indices[i + 1]].page == page && mesh.vertices[mesh.indices[i + 2]].page == page);
			}
			boxes.push_back(box);
		}
		const auto& large = atlas->meshes.back();
		for (std::size_t i = 0; i + 1 < boxes.size(); ++i) {
			for (const auto& vertex : large.vertices) assert(vertex.page != boxes[i].page);
			for (std::size_t j = i + 1; j + 1 < boxes.size(); ++j) {
				const auto& a = boxes[i];
				const auto& b = boxes[j];
				const bool apart = a.page != b.page || a.maxX <= b.minX || b.maxX <= a.minX || a.maxY <= b.minY || b.maxY <= a.minY;
				assert(apart);
			}
		}

		const auto again = BuildAtlas(meshes, options, &error);
		assert(again && again->pageCount == atlas->pageCount);
		for (std::size_t m = 0; m < meshes.size(); ++m) {
			for (std::size_t v = 0; v < atlas->meshes[m].vertices.size(); ++v) {
				const auto& a = atlas->meshes[m].vertices[v];
				const auto& b = again->meshes[m].vertices[v];
				assert(a.page == b.page && a.uv.x == b.uv.x && a.uv.y == b.uv.y && a.source == b.source);
			}
		}

		// Without a density there's nothing to size each mesh by.
		assert(!BuildAtlas(meshes, AtlasOptions{ .resolution = 256, .packing = Packing::PerMesh }, &error) && !error.empty());
	}
	// Many rectangles: none over another (padding kept), each inside its page and on 4-texel blocks, the same every time;
	// and one page as large as needed holds them all.
	void RectanglesPackApart()
	{
		std::vector<Rect> rects;
		std::uint32_t state = 7;
		for (int i = 0; i < 600; ++i) {
			state = state * 1664525u + 1013904223u;
			const auto w = 4 + (state >> 8) % 300;
			state = state * 1664525u + 1013904223u;
			const auto h = 4 + (state >> 8) % 200;
			rects.push_back({ w, h });
		}
		rects.push_back({ 1000, 900 });
		for (const auto resolution : { 1024u, 0u }) {
			std::string error;
			const auto pack = PackRects(rects, resolution, 4, &error);
			assert(pack && pack->placements.size() == rects.size());
			if (resolution == 0) assert(pack->pageCount == 1);
			else assert(pack->pageCount > 1 && pack->width == resolution && pack->height == resolution);
			for (std::size_t i = 0; i < rects.size(); ++i) {
				const auto& a = pack->placements[i];
				assert(a.x % 4 == 0 && a.y % 4 == 0 && a.page < pack->pageCount);
				assert(a.x + rects[i].width <= pack->width && a.y + rects[i].height <= pack->height);
			}
			for (std::size_t i = 0; i < rects.size(); ++i) {
				for (std::size_t j = i + 1; j < rects.size(); ++j) {
					const auto& a = pack->placements[i];
					const auto& b = pack->placements[j];
					const bool apart = a.page != b.page || a.x + rects[i].width + 4 <= b.x || b.x + rects[j].width + 4 <= a.x ||
					                   a.y + rects[i].height + 4 <= b.y || b.y + rects[j].height + 4 <= a.y;
					assert(apart);
				}
			}
			const auto again = PackRects(rects, resolution, 4, &error);
			for (std::size_t i = 0; i < rects.size(); ++i) {
				assert(again->placements[i].page == pack->placements[i].page && again->placements[i].x == pack->placements[i].x &&
				       again->placements[i].y == pack->placements[i].y);
			}
		}
		// Larger than a page: refused.
		std::string error;
		const Rect huge[] = { { 2000, 10 } };
		assert(!PackRects(huge, 1024, 4, &error) && !error.empty());
	}
}

int main()
{
	// A unit cube, its faces unshared.
	std::vector<Vec3> positions;
	std::vector<Vec3> normals;
	std::vector<std::uint32_t> indices;
	const Vec3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	for (int axis = 0; axis < 3; ++axis) {
		for (const float side : { -1.0f, 1.0f }) {
			const Vec3 n = axes[axis] * side;
			const Vec3 u = axes[(axis + 1) % 3];
			const Vec3 v = axes[(axis + 2) % 3];
			const auto base = static_cast<std::uint32_t>(positions.size());
			for (const auto& [a, b] : { std::pair{ -1, -1 }, std::pair{ 1, -1 }, std::pair{ 1, 1 }, std::pair{ -1, 1 } }) {
				positions.push_back(n * 0.5f + u * (0.5f * a) + v * (0.5f * b));
				normals.push_back(n);
			}
			indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}
	}

	const ChartMesh cube{ .positions = positions, .normals = normals, .indices = indices };
	const ChartMesh denser{ .positions = positions, .normals = normals, .indices = indices, .density = 2.0f };
	const ChartMesh meshes[] = { cube, denser };
	std::string error;
	const auto atlas = BuildAtlas(meshes, AtlasOptions{ .resolution = 256, .padding = 2, .texelsPerUnit = 32.0f }, &error);
	assert(atlas && error.empty());
	assert(atlas->width == 256 && atlas->pageCount >= 1 && atlas->meshes.size() == 2);
	for (const auto& mesh : atlas->meshes) {
		assert(mesh.indices.size() == indices.size());
		for (const auto& vertex : mesh.vertices) {
			assert(vertex.source < positions.size());
			assert(vertex.uv.x >= 0.0f && vertex.uv.x <= 1.0f && vertex.uv.y >= 0.0f && vertex.uv.y <= 1.0f);
		}
	}
	// Twice the density covers about four times the texels.
	const auto areaOf = [&](const OutMesh& a_mesh) {
		float area = 0;
		for (std::size_t i = 0; i < a_mesh.indices.size(); i += 3) {
			const auto& a = a_mesh.vertices[a_mesh.indices[i]].uv;
			const auto& b = a_mesh.vertices[a_mesh.indices[i + 1]].uv;
			const auto& c = a_mesh.vertices[a_mesh.indices[i + 2]].uv;
			area += std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5f;
		}
		return area;
	};
	const float ratio = areaOf(atlas->meshes[1]) / areaOf(atlas->meshes[0]);
	assert(ratio > 3.0f && ratio < 5.0f);

	PerMeshPackingPlacesMeshesApart(positions, normals, indices);
	RectanglesPackApart();

	const Rect rects[] = { { 64, 32 }, { 32, 32 }, { 100, 20 }, { 16, 80 } };
	const auto pack = PackRects(rects, 256, 2, &error);
	assert(pack && pack->placements.size() == 4);
	for (std::size_t i = 0; i < 4; ++i) {
		const auto& a = pack->placements[i];
		assert(a.x + rects[i].width <= pack->width && a.y + rects[i].height <= pack->height);
		for (std::size_t j = i + 1; j < 4; ++j) {
			const auto& b = pack->placements[j];
			const bool apart = a.page != b.page || a.x + rects[i].width <= b.x || b.x + rects[j].width <= a.x || a.y + rects[i].height <= b.y ||
			                   b.y + rects[j].height <= a.y;
			assert(apart);
		}
	}

	// A right triangle over half of a 10x10 square: about 50 texel centres inside, more with a 1-texel apron.
	int inside = 0, apron = 0;
	RasterizeTriangle({ 0, 0 }, { 10, 0 }, { 0, 10 }, 16, 16, [&](std::uint32_t, std::uint32_t, Vec3 a_barycentric, bool a_inside) {
		assert(std::abs(a_barycentric.x + a_barycentric.y + a_barycentric.z - 1.0f) < 1e-4f);
		(a_inside ? inside : apron)++;
	}, 1.0f);
	assert(inside >= 45 && inside <= 55 && apron > 10);

	std::cout << "tesworld atlas tests passed\n";
	return 0;
}
