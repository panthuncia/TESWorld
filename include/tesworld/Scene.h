#pragma once

#include "tesworld/Terrain.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tesworld
{
	// LODSettings\<worldspace>.lod: where a worldspace's LOD blocks start and how far they reach. The game aligns every
	// LOD block to its SW cell, so generated LOD must use the file the game will read.
	struct LodSettings
	{
		CellCoord swCell{};
		std::int32_t stride{ 0 };
		std::int32_t minLevel{ 4 };
		std::int32_t maxLevel{ 32 };

		[[nodiscard]] static std::optional<LodSettings> Parse(std::span<const std::uint8_t> a_bytes);
		[[nodiscard]] std::array<std::uint8_t, 16> Serialize() const;

		// Settings for a worldspace that has no file: the SW corner on a 32-cell boundary below its cells, and a
		// power-of-two stride that covers them.
		[[nodiscard]] static LodSettings Covering(CellCoord a_minCell, CellCoord a_maxCell);

		// The SW cell of the level's block holding a cell.
		[[nodiscard]] CellCoord BlockForCell(CellCoord a_cell, std::int32_t a_level) const;
		[[nodiscard]] bool Contains(CellCoord a_cell) const;
	};

	// One LOD block: a level (4, 8, 16 or 32) and its SW cell. Covers level x level cells.
	struct QuadKey
	{
		std::int32_t level{ 4 };
		std::int32_t x{ 0 };
		std::int32_t y{ 0 };

		friend bool operator==(const QuadKey&, const QuadKey&) = default;
		friend auto operator<=>(const QuadKey&, const QuadKey&) = default;
	};

	struct QuadKeyHash
	{
		std::size_t operator()(const QuadKey& a_key) const noexcept
		{
			return CellCoordHash{}(CellCoord{ a_key.x, a_key.y }) ^ (static_cast<std::size_t>(a_key.level) << 1);
		}
	};

	// The level's blocks that overlap the inclusive cell rectangle, west to east, south to north.
	[[nodiscard]] std::vector<QuadKey> QuadsCovering(const LodSettings& a_settings, std::int32_t a_level, CellCoord a_minCell, CellCoord a_maxCell);

	[[nodiscard]] inline CellCoord CellAt(float a_x, float a_y)
	{
		return { static_cast<std::int32_t>(std::floor(a_x / kSkyrimTerrainCellSize)), static_cast<std::int32_t>(std::floor(a_y / kSkyrimTerrainCellSize)) };
	}
}
