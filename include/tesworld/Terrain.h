#pragma once

#include "tesworld/Math.h"
#include "tesworld/Plugins.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

namespace tesworld
{
	inline constexpr std::int32_t kLandVerticesPerCell = 32;
	inline constexpr float kLandVertexSpacing = kSkyrimTerrainCellSize / kLandVerticesPerCell;

	struct CellCoord
	{
		std::int32_t x{ 0 };
		std::int32_t y{ 0 };

		friend bool operator==(const CellCoord&, const CellCoord&) = default;
	};

	struct CellCoordHash
	{
		std::size_t operator()(CellCoord a_cell) const noexcept
		{
			return (static_cast<std::size_t>(static_cast<std::uint32_t>(a_cell.x)) * 0x9E3779B1u) ^
			       (static_cast<std::size_t>(static_cast<std::uint32_t>(a_cell.y)) * 0x85EBCA77u);
		}
	};

	// A worldspace's exterior cells by grid: never an interior cell, nor its persistent cell, which has a grid (0,0 in
	// Skyrim.esm) but stands for no place; of several at one grid, the lowest FormID's. Every reading of a world's cells
	// by grid goes through it, so none of them takes the persistent cell for the one at 0,0.
	[[nodiscard]] std::unordered_map<CellCoord, const CellInfo*, CellCoordHash> ExteriorCells(const StaticWorldSnapshot& a_snapshot, FormID a_worldFormID);

	// A worldspace's bounds (NAM0/NAM9) as the inclusive cells they cover; none when it has none, or only an empty
	// placeholder (min equal to max on an axis), or bounds that aren't finite or are inverted: then its LAND gives the extent.
	[[nodiscard]] std::optional<std::pair<CellCoord, CellCoord>> CellBounds(const WorldInfo& a_world);

	// One worldspace's landscape on the game's vertex lattice: a vertex every 128 units, 32 per cell, so lattice
	// point (gx, gy) is at world (gx * 128, gy * 128). Cells without LAND (or LAND without heights) take the worldspace's
	// default land height. A worldspace that uses its parent's land data reads the parent's LAND. Reads the snapshot it
	// was built from, which must outlive it.
	class WorldTerrain
	{
	public:
		WorldTerrain() = default;
		WorldTerrain(const StaticWorldSnapshot& a_snapshot, FormID a_worldFormID);

		[[nodiscard]] FormID World() const { return _world; }
		[[nodiscard]] FormID LandWorld() const { return _landWorld; }
		[[nodiscard]] FormID WaterWorld() const { return _waterWorld; }
		[[nodiscard]] float DefaultLandHeight() const { return _defaultLandHeight; }
		[[nodiscard]] float DefaultWaterHeight() const { return _defaultWaterHeight; }

		// The cells that have LAND, as an inclusive rectangle; empty when there are none.
		[[nodiscard]] bool HasLand() const { return !_lands.empty(); }
		[[nodiscard]] CellCoord MinCell() const { return _minCell; }
		[[nodiscard]] CellCoord MaxCell() const { return _maxCell; }

		[[nodiscard]] const LandInfo* Land(CellCoord a_cell) const;
		[[nodiscard]] float HeightAt(std::int32_t a_gx, std::int32_t a_gy) const;
		// Bilinear height at a world position.
		[[nodiscard]] float SampleHeight(float a_x, float a_y) const;
		// The landscape normal at a lattice point, from the heights (LAND's own normals are ignored, as the game's LOD
		// does).
		[[nodiscard]] Vec3 NormalAt(std::int32_t a_gx, std::int32_t a_gy) const;
		// LAND vertex colour (VCLR) at a lattice point, 0..1; white without one.
		[[nodiscard]] Vec3 ColorAt(std::int32_t a_gx, std::int32_t a_gy) const;
		// A cell's water height: its own XCLW, else the worldspace default.
		[[nodiscard]] float WaterHeight(CellCoord a_cell) const;

	private:
		[[nodiscard]] std::pair<const LandInfo*, std::size_t> Vertex(std::int32_t a_gx, std::int32_t a_gy) const;

		FormID _world{};
		FormID _landWorld{};
		FormID _waterWorld{};
		float _defaultLandHeight{ -2048.0f };
		float _defaultWaterHeight{ 0.0f };
		CellCoord _minCell{};
		CellCoord _maxCell{};
		std::unordered_map<CellCoord, const LandInfo*, CellCoordHash> _lands;
		std::unordered_map<CellCoord, float, CellCoordHash> _waterHeights;
	};
}