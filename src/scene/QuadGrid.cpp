#include "tesworld/Scene.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace tesworld
{
	std::optional<LodSettings> LodSettings::Parse(std::span<const std::uint8_t> a_bytes)
	{
		if (a_bytes.size() != 16) {
			return std::nullopt;
		}
		std::int16_t swX{}, swY{};
		LodSettings settings;
		std::memcpy(&swX, a_bytes.data() + 0, 2);
		std::memcpy(&swY, a_bytes.data() + 2, 2);
		std::memcpy(&settings.stride, a_bytes.data() + 4, 4);
		std::memcpy(&settings.minLevel, a_bytes.data() + 8, 4);
		std::memcpy(&settings.maxLevel, a_bytes.data() + 12, 4);
		settings.swCell = { swX, swY };
		if (settings.stride <= 0 || settings.minLevel <= 0 || settings.maxLevel < settings.minLevel) {
			return std::nullopt;
		}
		return settings;
	}

	std::array<std::uint8_t, 16> LodSettings::Serialize() const
	{
		std::array<std::uint8_t, 16> bytes{};
		const auto swX = static_cast<std::int16_t>(swCell.x);
		const auto swY = static_cast<std::int16_t>(swCell.y);
		std::memcpy(bytes.data() + 0, &swX, 2);
		std::memcpy(bytes.data() + 2, &swY, 2);
		std::memcpy(bytes.data() + 4, &stride, 4);
		std::memcpy(bytes.data() + 8, &minLevel, 4);
		std::memcpy(bytes.data() + 12, &maxLevel, 4);
		return bytes;
	}

	LodSettings LodSettings::Covering(CellCoord a_minCell, CellCoord a_maxCell)
	{
		LodSettings settings;
		settings.swCell = { FloorDiv(a_minCell.x, 32) * 32, FloorDiv(a_minCell.y, 32) * 32 };
		const auto extent = (std::max)({ a_maxCell.x - settings.swCell.x + 1, a_maxCell.y - settings.swCell.y + 1, 32 });
		settings.stride = static_cast<std::int32_t>(std::bit_ceil(static_cast<std::uint32_t>(extent)));
		return settings;
	}

	CellCoord LodSettings::BlockForCell(CellCoord a_cell, std::int32_t a_level) const
	{
		return {
			swCell.x + FloorDiv(a_cell.x - swCell.x, a_level) * a_level,
			swCell.y + FloorDiv(a_cell.y - swCell.y, a_level) * a_level,
		};
	}

	bool LodSettings::Contains(CellCoord a_cell) const
	{
		return a_cell.x >= swCell.x && a_cell.y >= swCell.y && a_cell.x < swCell.x + stride && a_cell.y < swCell.y + stride;
	}

	std::vector<QuadKey> QuadsCovering(const LodSettings& a_settings, std::int32_t a_level, CellCoord a_minCell, CellCoord a_maxCell)
	{
		std::vector<QuadKey> quads;
		if (a_maxCell.x < a_minCell.x || a_maxCell.y < a_minCell.y) {
			return quads;
		}
		const auto first = a_settings.BlockForCell({ (std::max)(a_minCell.x, a_settings.swCell.x), (std::max)(a_minCell.y, a_settings.swCell.y) }, a_level);
		const auto last = a_settings.BlockForCell(a_maxCell, a_level);
		for (auto y = first.y; y <= last.y; y += a_level) {
			for (auto x = first.x; x <= last.x; x += a_level) {
				quads.push_back(QuadKey{ a_level, x, y });
			}
		}
		return quads;
	}
}
