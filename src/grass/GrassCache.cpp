#include "tesworld/GrassCache.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace tesworld::grass
{
	namespace
	{
		// More than this isn't a cache: a cell holds a few dozen grass types.
		constexpr std::uint32_t kMaxGroups = 4096;

		class Reader
		{
		public:
			explicit Reader(std::span<const std::uint8_t> a_bytes) :
				_bytes(a_bytes)
			{}

			bool Has(std::size_t a_count) const { return _bytes.size() - _offset >= a_count; }

			std::uint8_t U8() { return _bytes[_offset++]; }

			std::uint32_t U32()
			{
				std::uint32_t value = 0;
				std::memcpy(&value, _bytes.data() + _offset, 4);
				_offset += 4;
				return value;
			}

			std::uint16_t U16At(std::size_t a_offset) const
			{
				return static_cast<std::uint16_t>(_bytes[a_offset] | (static_cast<std::uint16_t>(_bytes[a_offset + 1]) << 8));
			}

			std::string_view Text(std::size_t a_length)
			{
				const std::string_view text(reinterpret_cast<const char*>(_bytes.data() + _offset), a_length);
				_offset += a_length;
				return text;
			}

			std::size_t Offset() const { return _offset; }
			void Skip(std::size_t a_count) { _offset += a_count; }

		private:
			std::span<const std::uint8_t> _bytes;
			std::size_t _offset{ 0 };
		};

		void PutU32(std::vector<std::uint8_t>& a_out, std::uint32_t a_value)
		{
			for (int i = 0; i < 4; ++i) a_out.push_back(static_cast<std::uint8_t>(a_value >> (8 * i)));
		}

		void PutU16(std::vector<std::uint8_t>& a_out, std::uint16_t a_value)
		{
			a_out.push_back(static_cast<std::uint8_t>(a_value));
			a_out.push_back(static_cast<std::uint8_t>(a_value >> 8));
		}
	}

	std::string CacheFilePath(std::string_view a_world, std::int32_t a_cellX, std::int32_t a_cellY, std::string_view a_extension)
	{
		return std::format("grass\\{}x{:04}y{:04}.{}", a_world, a_cellX, a_cellY, a_extension);
	}

	float BlockBase(std::int32_t a_cell)
	{
		return static_cast<float>((a_cell / kBlockCells) * kBlockCells) * kSkyrimTerrainCellSize;
	}

	float HalfToFloat(std::uint16_t a_half)
	{
		const std::uint32_t sign = (a_half & 0x8000u) << 16;
		const std::uint32_t exponent = (a_half >> 10) & 0x1Fu;
		std::uint32_t mantissa = a_half & 0x3FFu;
		if (exponent == 0x1F) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
		if (exponent != 0) return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
		if (mantissa == 0) return std::bit_cast<float>(sign);
		// Subnormal: mantissa * 2^-24.
		const float value = static_cast<float>(mantissa) * 5.9604645e-8f;
		return sign != 0 ? -value : value;
	}

	std::uint16_t FloatToHalf(float a_value)
	{
		const auto bits = std::bit_cast<std::uint32_t>(a_value);
		const auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
		const float magnitude = std::abs(a_value);
		if (std::isnan(a_value)) return static_cast<std::uint16_t>(sign | 0x7E00u);
		if (magnitude >= 65520.0f) return static_cast<std::uint16_t>(sign | 0x7C00u);
		if (magnitude < 6.1035156e-5f) {
			return static_cast<std::uint16_t>(sign | static_cast<std::uint16_t>(std::lround(magnitude / 5.9604645e-8f)));
		}
		// Round to nearest even on the 13 dropped bits.
		const auto absBits = bits & 0x7FFFFFFFu;
		auto half = static_cast<std::uint32_t>(((absBits >> 23) - 112) << 10) | ((absBits >> 13) & 0x3FFu);
		const auto dropped = absBits & 0x1FFFu;
		if (dropped > 0x1000u || (dropped == 0x1000u && (half & 1u) != 0)) ++half;
		return static_cast<std::uint16_t>(sign | half);
	}

	std::optional<CellCache> ReadCellCache(std::span<const std::uint8_t> a_bytes, std::int32_t a_cellX, std::int32_t a_cellY, bool a_blades, std::string* a_error)
	{
		const auto fail = [&](std::string a_why) -> std::optional<CellCache> {
			if (a_error) *a_error = std::move(a_why);
			return std::nullopt;
		};
		Reader in(a_bytes);
		if (!in.Has(4)) return fail("shorter than its header");
		const auto groups = in.U32();
		if (groups > kMaxGroups) return fail(std::format("{} groups", groups));
		const float baseX = BlockBase(a_cellX), baseY = BlockBase(a_cellY);
		CellCache cache;
		cache.groups.reserve(groups);
		for (std::uint32_t g = 0; g < groups; ++g) {
			if (!in.Has(4)) return fail(std::format("group {} is cut off", g));
			const auto length = in.U32();
			if (length == 0 || !in.Has(static_cast<std::size_t>(length) + 15)) return fail(std::format("group {} is cut off", g));
			Group group;
			auto path = in.Text(length);
			if (const auto end = path.find('\0'); end != std::string_view::npos) path = path.substr(0, end);
			group.modelPath = std::string(path);
			group.wavePeriod = std::bit_cast<float>(in.U32());
			group.grass = FormID{ in.U32() };
			group.vertexLighting = in.U8() != 0;
			group.uniformScaling = in.U8() != 0;
			group.fitToSlope = in.U8() != 0;
			const auto blocks = in.U32();
			for (std::uint32_t b = 0; b < blocks; ++b) {
				if (!in.Has(36)) return fail(std::format("group {} block {} is cut off", g, b));
				in.Skip(28);
				const auto count = in.U32();
				const auto words = in.U32();
				if (words < 14 || words > 64) return fail(std::format("group {} block {} has {} words a blade", g, b, words));
				const auto bytes = static_cast<std::size_t>(count) * words * 2;
				if (!in.Has(bytes)) return fail(std::format("group {} block {} is cut off", g, b));
				group.bladeCount += count;
				if (a_blades) {
					const auto start = in.Offset();
					const auto word = [&](std::uint32_t a_blade, std::uint32_t a_word) {
						return HalfToFloat(in.U16At(start + (static_cast<std::size_t>(a_blade) * words + a_word) * 2));
					};
					for (std::uint32_t i = 0; i < count; ++i) {
						Blade blade;
						blade.position = { word(i, 0) + baseX, word(i, 1) + baseY, word(i, 2) };
						blade.brightness = word(i, 3);
						// The rows are (4, 5, 6), (8, 9, 10), (12, 7, 11); the basis vectors are the columns.
						blade.basis[0] = { word(i, 4), word(i, 8), word(i, 12) };
						blade.basis[1] = { word(i, 5), word(i, 9), word(i, 7) };
						blade.basis[2] = { word(i, 6), word(i, 10), word(i, 11) };
						blade.scale = 1.0f + word(i, 13);
						group.blades.push_back(blade);
					}
				}
				in.Skip(bytes);
			}
			cache.groups.push_back(std::move(group));
		}
		return cache;
	}

	std::vector<std::uint8_t> WriteCellCache(const CellCache& a_cache, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		const float baseX = BlockBase(a_cellX), baseY = BlockBase(a_cellY);
		std::vector<std::uint8_t> out;
		PutU32(out, static_cast<std::uint32_t>(a_cache.groups.size()));
		for (const auto& group : a_cache.groups) {
			PutU32(out, static_cast<std::uint32_t>(group.modelPath.size() + 1));
			out.insert(out.end(), group.modelPath.begin(), group.modelPath.end());
			out.push_back(0);
			PutU32(out, std::bit_cast<std::uint32_t>(group.wavePeriod));
			PutU32(out, group.grass.value);
			out.push_back(group.vertexLighting ? 1 : 0);
			out.push_back(group.uniformScaling ? 1 : 0);
			out.push_back(group.fitToSlope ? 1 : 0);
			PutU32(out, group.blades.empty() ? 0u : 1u);
			if (group.blades.empty()) continue;
			// The block's bounds, padded by 30 units as the engine pads them.
			Vec3 min{ (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)() };
			Vec3 max{ -min.x, -min.y, -min.z };
			for (const auto& blade : group.blades) {
				min = { (std::min)(min.x, blade.position.x), (std::min)(min.y, blade.position.y), (std::min)(min.z, blade.position.z) };
				max = { (std::max)(max.x, blade.position.x), (std::max)(max.y, blade.position.y), (std::max)(max.z, blade.position.z) };
			}
			for (const float value : { (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f, (max.x - min.x) * 0.5f + 30.0f,
					 (max.y - min.y) * 0.5f + 30.0f, (max.z - min.z) * 0.5f + 30.0f }) {
				PutU32(out, std::bit_cast<std::uint32_t>(value));
			}
			PutU32(out, 0);
			PutU32(out, static_cast<std::uint32_t>(group.blades.size()));
			PutU32(out, kBladeWords);
			for (const auto& blade : group.blades) {
				const auto& [x, y, up] = blade.basis;
				const float words[kBladeWords]{ blade.position.x - baseX, blade.position.y - baseY, blade.position.z, blade.brightness, x.x, y.x, up.x, y.z, x.y, y.y,
					up.y, up.z, x.z, blade.scale - 1.0f, 0.0f, 0.0f };
				for (const float word : words) PutU16(out, FloatToHalf(word));
			}
		}
		return out;
	}
}