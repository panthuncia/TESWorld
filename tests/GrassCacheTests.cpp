// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/GrassCache.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace tesworld;
using namespace tesworld::grass;

namespace
{
	bool Near(float a, float b, float tolerance = 1e-3f) { return std::abs(a - b) <= tolerance; }

	void HalfFloatsRoundTrip()
	{
		for (const float value : { 0.0f, 1.0f, -1.0f, 0.5f, 0.333f, 1024.0f, -7596.0f, 41184.0f, 6.0e-5f, 65504.0f }) {
			const auto back = HalfToFloat(FloatToHalf(value));
			assert(std::abs(back - value) <= std::abs(value) * 0.001f + 1e-7f);
		}
		assert(FloatToHalf(1.0f) == 0x3C00 && FloatToHalf(-2.0f) == 0xC000 && HalfToFloat(0x7BFF) == 65504.0f);
		// Halfway rounds to even: 2049 lies between 2048 and 2050.
		assert(HalfToFloat(FloatToHalf(2049.0f)) == 2048.0f && HalfToFloat(FloatToHalf(2051.0f)) == 2052.0f);
	}

	void NamesAndBlocksFollowNgio()
	{
		assert(CacheFilePath("Tamriel", 10, -6) == "grass\\Tamrielx0010y-006.cgid");
		assert(CacheFilePath("Tamriel", -1, -14, "gid") == "grass\\Tamrielx-001y-014.gid");
		// Blocks of 12 cells, counted toward zero: cells -11..11 share the corner at 0.
		assert(BlockBase(10) == 0.0f && BlockBase(-11) == 0.0f && BlockBase(12) == 49152.0f && BlockBase(-14) == -49152.0f);
	}

	void CellsRoundTrip()
	{
		CellCache cache;
		Group grass{ .modelPath = "landscape\\grass\\fieldgrass01.nif", .grass = FormID{ 0x000135BE }, .wavePeriod = 250.0f, .vertexLighting = true,
			.uniformScaling = true };
		// Turned a quarter about Z, and one on a slope.
		grass.blades.push_back(Blade{ .position = { -57000.0f, 41000.0f, -7596.0f },
			.basis = { Vec3{ 0, 1, 0 }, Vec3{ -1, 0, 0 }, Vec3{ 0, 0, 1 } }, .scale = 1.25f, .brightness = 0.875f });
		grass.blades.push_back(Blade{ .position = { -56000.0f, 41500.0f, 120.0f },
			.basis = { Vec3{ 1, 0, 0 }, Vec3{ 0, 0.8f, -0.6f }, Vec3{ 0, 0.6f, 0.8f } } });
		cache.groups.push_back(grass);
		cache.groups.push_back(Group{ .modelPath = "landscape\\grass\\fern.nif", .grass = FormID{ 0x80000801 } });
		const auto bytes = WriteCellCache(cache, -14, 10);

		std::string error;
		const auto read = ReadCellCache(bytes, -14, 10, true, &error);
		assert(read && read->groups.size() == 2);
		const auto& first = read->groups[0];
		assert(first.modelPath == grass.modelPath && first.grass == grass.grass && first.wavePeriod == 250.0f);
		assert(first.vertexLighting && first.uniformScaling && !first.fitToSlope && first.bladeCount == 2 && first.blades.size() == 2);
		// x and y are stored from the block's corner, (-49152, 0); z is absolute. Halves keep about 11 bits.
		const auto& turned = first.blades[0];
		assert(Near(turned.position.x, -57000.0f, 4.0f) && Near(turned.position.y, 41000.0f, 16.0f) && Near(turned.position.z, -7596.0f, 4.0f));
		assert(Near(turned.basis[0].y, 1.0f) && Near(turned.basis[1].x, -1.0f) && Near(turned.basis[2].z, 1.0f));
		assert(Near(turned.scale, 1.25f) && Near(turned.brightness, 0.875f));
		const auto& sloped = first.blades[1];
		assert(Near(sloped.basis[1].z, -0.6f) && Near(sloped.basis[2].y, 0.6f) && Near(sloped.basis[2].z, 0.8f) && Near(sloped.scale, 1.0f));
		assert(read->groups[1].bladeCount == 0 && read->groups[1].grass == FormID{ 0x80000801 });

		// Headers only: the counts without the blades.
		const auto headers = ReadCellCache(bytes, -14, 10, false);
		assert(headers && headers->groups[0].bladeCount == 2 && headers->groups[0].blades.empty());

		// What isn't a cache says so.
		assert(!ReadCellCache(std::span(bytes).first(bytes.size() - 1), -14, 10, true, &error) && error.find("cut off") != std::string::npos);
		assert(!ReadCellCache(std::span(bytes).first(2), -14, 10));
		const std::vector<std::uint8_t> many{ 0xFF, 0xFF, 0, 0 };
		assert(!ReadCellCache(many, 0, 0));
	}
}

int main()
{
	HalfFloatsRoundTrip();
	NamesAndBlocksFollowNgio();
	CellsRoundTrip();
	std::cout << "grass cache tests passed\n";
	return 0;
}