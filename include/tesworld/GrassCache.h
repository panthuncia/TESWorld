#pragma once

#include "tesworld/Math.h"
#include "tesworld/Plugins.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// No Grass In Objects' grass cache: one file per exterior cell, Data\Grass\<world>x<XXXX>y<YYYY>.cgid (".gid" for older
// caches, the same layout). It holds the blades the game would place, as the engine's grass shapes store them, so a
// tool reading it sees the same grass the game draws.
//
// The layout, little-endian:
//   u32 groups
//   per group (one grass type):
//     u32 length, then the model path and a 0 (length counts the 0)
//     f32 wave period, u32 grass FormID (as the game numbered it at run time: the load order the cache was made with)
//     u8 vertex lighting, u8 uniform scaling, u8 fit to slope
//     u32 blocks
//     per block: 9 u32 (f32 centre xyz, f32 half extent xyz, an unused count, u32 blades, u32 words per blade = 16),
//       then blades * words half floats per blade:
//         0-2   position: x and y from the corner of the cell's 12x12-cell block, z absolute
//         3     brightness
//         4-12  the rotation, whose rows are (4, 5, 6), (8, 9, 10), (12, 7, 11)
//         13    scale - 1
//         14-15 unused
namespace tesworld::grass
{
	inline constexpr std::uint32_t kBladeWords = 16;
	// Cells share a block corner in 12 x 12 groups, counted from cell 0 toward zero (C++'s division).
	inline constexpr std::int32_t kBlockCells = 12;

	// One blade, in game space.
	struct Blade
	{
		Vec3 position{};
		// Where the model's x, y and up axes go (the rotation's columns).
		std::array<Vec3, 3> basis{ Vec3{ 1, 0, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 0, 1 } };
		// The model's scale: across all axes with uniform scaling, else its height only.
		float scale{ 1.0f };
		float brightness{ 1.0f };
	};

	struct Group
	{
		std::string modelPath;
		FormID grass{};
		float wavePeriod{ 0.0f };
		bool vertexLighting{ false };
		bool uniformScaling{ false };
		bool fitToSlope{ false };
		std::uint32_t bladeCount{ 0 };
		// Empty when read without blades.
		std::vector<Blade> blades;
	};

	struct CellCache
	{
		std::vector<Group> groups;
	};

	// "<world>x<XXXX>y<YYYY>.<extension>" under grass\, as NGIO names a cell's cache (x and y signed, four digits).
	[[nodiscard]] std::string CacheFilePath(std::string_view a_world, std::int32_t a_cellX, std::int32_t a_cellY, std::string_view a_extension = "cgid");

	// The corner of the cache block a cell belongs to, in game units (one axis).
	[[nodiscard]] float BlockBase(std::int32_t a_cell);

	// A cell's cache; with a_blades false, only each group's header and blade count. Null, with why, when the bytes
	// aren't a cache.
	[[nodiscard]] std::optional<CellCache> ReadCellCache(std::span<const std::uint8_t> a_bytes, std::int32_t a_cellX, std::int32_t a_cellY, bool a_blades = true,
		std::string* a_error = nullptr);

	// The bytes of a cell's cache, one block per group, for tests and tools.
	[[nodiscard]] std::vector<std::uint8_t> WriteCellCache(const CellCache& a_cache, std::int32_t a_cellX, std::int32_t a_cellY);

	[[nodiscard]] float HalfToFloat(std::uint16_t a_half);
	[[nodiscard]] std::uint16_t FloatToHalf(float a_value);
}