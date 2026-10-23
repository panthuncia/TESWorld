#include "tesworld/Terrain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tesworld
{
	WorldTerrain::WorldTerrain(const StaticWorldSnapshot& a_snapshot, FormID a_worldFormID) :
		_world(a_worldFormID),
		_landWorld(a_worldFormID)
	{
		// Each parent-use flag is inherited independently. Missing parents stop the chain; cycles stop at
		// the repeated world and are reported as unresolved by the differential validator.
		const auto source = [&](std::uint16_t flag) {
			auto current = a_worldFormID;
			std::vector<FormID> visited;
			for (;;) {
				const auto it = a_snapshot.worldsByFormID.find(current);
				if (it == a_snapshot.worldsByFormID.end() || !it->second.UsesParent(flag)) return current;
				if (std::ranges::find(visited, current) != visited.end()) return current;
				visited.push_back(current);
				if (!a_snapshot.worldsByFormID.contains(*it->second.parentWorld)) return current;
				current = *it->second.parentWorld;
			}
		};
		_landWorld = source(WorldInfo::ParentUseLandData);
		_waterWorld = source(WorldInfo::ParentUseWaterData);
		const auto landIt = a_snapshot.worldsByFormID.find(_landWorld);
		const auto waterIt = a_snapshot.worldsByFormID.find(_waterWorld);
		const auto* landWorld = landIt == a_snapshot.worldsByFormID.end() ? nullptr : &landIt->second;
		const auto* waterWorld = waterIt == a_snapshot.worldsByFormID.end() ? nullptr : &waterIt->second;
		if (landWorld && landWorld->defaultLandHeight) {
			_defaultLandHeight = *landWorld->defaultLandHeight;
		}
		if (waterWorld && waterWorld->defaultWaterHeight) {
			_defaultWaterHeight = *waterWorld->defaultWaterHeight;
		}

		_minCell = { (std::numeric_limits<std::int32_t>::max)(), (std::numeric_limits<std::int32_t>::max)() };
		_maxCell = { (std::numeric_limits<std::int32_t>::min)(), (std::numeric_limits<std::int32_t>::min)() };
		if (const auto it = a_snapshot.landsByWorldspace.find(_landWorld); it != a_snapshot.landsByWorldspace.end()) {
			_lands.reserve(it->second.size());
			for (const auto& land : it->second) {
				if (!land.cellX || !land.cellY) {
					continue;
				}
				const CellCoord cell{ *land.cellX, *land.cellY };
				_lands.insert_or_assign(cell, std::addressof(land));
				_minCell = { (std::min)(_minCell.x, cell.x), (std::min)(_minCell.y, cell.y) };
				_maxCell = { (std::max)(_maxCell.x, cell.x), (std::max)(_maxCell.y, cell.y) };
			}
		}
		if (_lands.empty()) {
			_minCell = {};
			_maxCell = {};
		}

		for (const auto& [at, cell] : ExteriorCells(a_snapshot, waterWorld ? waterWorld->formID : a_worldFormID)) {
			if (cell->waterHeight) _waterHeights.emplace(at, *cell->waterHeight);
		}
	}

	std::optional<std::pair<CellCoord, CellCoord>> CellBounds(const WorldInfo& a_world)
	{
		if (!a_world.boundsMin || !a_world.boundsMax) return std::nullopt;
		const auto& min = *a_world.boundsMin;
		const auto& max = *a_world.boundsMax;
		for (int axis = 0; axis < 2; ++axis) {
			if (!std::isfinite(min[axis]) || !std::isfinite(max[axis]) || !(min[axis] < max[axis])) return std::nullopt;
		}
		const auto cell = [](float a_coordinate) { return static_cast<std::int32_t>(std::floor(a_coordinate / kSkyrimTerrainCellSize)); };
		// The far bound is exclusive: a world ending on a cell's edge doesn't take the cell beyond it.
		return std::pair{ CellCoord{ cell(min[0]), cell(min[1]) }, CellCoord{ cell(max[0] - 1.0f), cell(max[1] - 1.0f) } };
	}

	std::unordered_map<CellCoord, const CellInfo*, CellCoordHash> ExteriorCells(const StaticWorldSnapshot& a_snapshot, FormID a_worldFormID)
	{
		std::unordered_map<CellCoord, const CellInfo*, CellCoordHash> cells;
		for (const auto& [formID, cell] : a_snapshot.cellsByFormID) {
			if (cell.worldFormID != a_worldFormID || !cell.gridX || !cell.gridY || cell.IsInterior() || cell.IsPersistent()) continue;
			const auto [it, added] = cells.try_emplace(CellCoord{ *cell.gridX, *cell.gridY }, &cell);
			if (!added && formID.value < it->second->formID.value) it->second = &cell;
		}
		return cells;
	}

	const LandInfo* WorldTerrain::Land(CellCoord a_cell) const
	{
		const auto it = _lands.find(a_cell);
		return it == _lands.end() ? nullptr : it->second;
	}

	std::pair<const LandInfo*, std::size_t> WorldTerrain::Vertex(std::int32_t a_gx, std::int32_t a_gy) const
	{
		// A point on a cell edge belongs to both cells; their LAND agrees there, so take the cell it starts.
		const CellCoord cell{ FloorDiv(a_gx, kLandVerticesPerCell), FloorDiv(a_gy, kLandVerticesPerCell) };
		const auto localX = static_cast<std::size_t>(a_gx - cell.x * kLandVerticesPerCell);
		const auto localY = static_cast<std::size_t>(a_gy - cell.y * kLandVerticesPerCell);
		const auto* land = Land(cell);
		if (!land) {
			// The point may still be the far edge of the cell to its west or south.
			if (localX == 0 && localY == 0) {
				if (const auto* west = Land({ cell.x - 1, cell.y })) return { west, LandInfo::VertexSide - 1 };
				if (const auto* south = Land({ cell.x, cell.y - 1 })) return { south, (LandInfo::VertexSide - 1) * LandInfo::VertexSide };
				if (const auto* southWest = Land({ cell.x - 1, cell.y - 1 })) return { southWest, LandInfo::VertexCount - 1 };
			} else if (localX == 0) {
				if (const auto* west = Land({ cell.x - 1, cell.y })) return { west, localY * LandInfo::VertexSide + LandInfo::VertexSide - 1 };
			} else if (localY == 0) {
				if (const auto* south = Land({ cell.x, cell.y - 1 })) return { south, (LandInfo::VertexSide - 1) * LandInfo::VertexSide + localX };
			}
			return { nullptr, 0 };
		}
		return { land, localY * LandInfo::VertexSide + localX };
	}

	float WorldTerrain::HeightAt(std::int32_t a_gx, std::int32_t a_gy) const
	{
		const auto [land, index] = Vertex(a_gx, a_gy);
		return land && land->hasHeights ? land->heights[index] : _defaultLandHeight;
	}

	float WorldTerrain::SampleHeight(float a_x, float a_y) const
	{
		const float fx = a_x / kLandVertexSpacing;
		const float fy = a_y / kLandVertexSpacing;
		const auto gx = static_cast<std::int32_t>(std::floor(fx));
		const auto gy = static_cast<std::int32_t>(std::floor(fy));
		const float tx = fx - static_cast<float>(gx);
		const float ty = fy - static_cast<float>(gy);
		const float h00 = HeightAt(gx, gy), h10 = HeightAt(gx + 1, gy);
		const float h01 = HeightAt(gx, gy + 1), h11 = HeightAt(gx + 1, gy + 1);
		return (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
	}

	Vec3 WorldTerrain::NormalAt(std::int32_t a_gx, std::int32_t a_gy) const
	{
		const float dx = HeightAt(a_gx + 1, a_gy) - HeightAt(a_gx - 1, a_gy);
		const float dy = HeightAt(a_gx, a_gy + 1) - HeightAt(a_gx, a_gy - 1);
		return Normalize({ -dx, -dy, 2.0f * kLandVertexSpacing });
	}

	Vec3 WorldTerrain::ColorAt(std::int32_t a_gx, std::int32_t a_gy) const
	{
		const auto [land, index] = Vertex(a_gx, a_gy);
		if (!land || !land->hasVertexColors) {
			return { 1.0f, 1.0f, 1.0f };
		}
		const auto& color = land->vertexColors[index];
		return { color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f };
	}

	float WorldTerrain::WaterHeight(CellCoord a_cell) const
	{
		const auto it = _waterHeights.find(a_cell);
		return it == _waterHeights.end() ? _defaultWaterHeight : it->second;
	}
}