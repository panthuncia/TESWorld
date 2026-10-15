// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Math.h"
#include "tesworld/Scene.h"
#include "tesworld/Terrain.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <numbers>

using namespace tesworld;

namespace
{
	bool Near(float a, float b, float tolerance = 1e-3f) { return std::abs(a - b) <= tolerance; }

	LandInfo FlatLand(FormID a_formID, FormID a_cell, float a_height)
	{
		LandInfo land{};
		land.formID = a_formID;
		land.parentCell = a_cell;
		land.hasHeights = true;
		land.heights.fill(a_height);
		return land;
	}

	StaticWorldSnapshot MakeWorld()
	{
		StaticPluginShard shard{};
		shard.entry.pluginName = "test.esm";
		WorldInfo world{};
		world.formID = FormID{ 0x3C };
		world.defaultLandHeight = -2048.0f;
		world.defaultWaterHeight = -100.0f;
		shard.worlds.push_back(world);

		// Two cells side by side: (0,0) at height 100, (1,0) at 300 with water at 50.
		CellInfo a{ .formID = FormID{ 0x100 }, .worldFormID = FormID{ 0x3C }, .gridX = 0, .gridY = 0 };
		CellInfo b{ .formID = FormID{ 0x101 }, .worldFormID = FormID{ 0x3C }, .gridX = 1, .gridY = 0, .waterHeight = 50.0f };
		shard.cells = { a, b };
		shard.lands.push_back(FlatLand(FormID{ 0x200 }, FormID{ 0x100 }, 100.0f));
		auto east = FlatLand(FormID{ 0x201 }, FormID{ 0x101 }, 300.0f);
		east.hasVertexColors = true;
		for (auto& color : east.vertexColors) color = { 255, 0, 0 };
		shard.lands.push_back(east);
		return BuildStaticWorldSnapshot({ shard });
	}

	void TerrainSamplesLatticeAndDefaults()
	{
		const auto snapshot = MakeWorld();
		const WorldTerrain terrain(snapshot, FormID{ 0x3C });
		assert(terrain.HasLand());
		assert(terrain.MinCell() == (CellCoord{ 0, 0 }));
		assert(terrain.MaxCell() == (CellCoord{ 1, 0 }));
		assert(Near(terrain.HeightAt(5, 5), 100.0f));
		assert(Near(terrain.HeightAt(40, 5), 300.0f));
		// Lattice point 32 starts cell 1, which wins the shared edge.
		assert(Near(terrain.HeightAt(32, 5), 300.0f));
		// The far edge of the last cell reads that cell's LAND.
		assert(Near(terrain.HeightAt(64, 5), 300.0f));
		// No LAND: the worldspace default.
		assert(Near(terrain.HeightAt(5, 40), -2048.0f));
		assert(Near(terrain.HeightAt(-5, 5), -2048.0f));
		// Halfway between 100 and 300 across the edge between lattice points 31 and 32.
		assert(Near(terrain.SampleHeight(31.5f * 128.0f, 5 * 128.0f), 200.0f));
		assert(Near(terrain.WaterHeight({ 1, 0 }), 50.0f));
		assert(Near(terrain.WaterHeight({ 0, 0 }), -100.0f));
		const auto red = terrain.ColorAt(40, 5);
		assert(Near(red.x, 1.0f) && Near(red.y, 0.0f));
		const auto flat = terrain.NormalAt(5, 5);
		assert(Near(flat.z, 1.0f));
	}

	void ParentSourcesFollowIndependentChains()
	{
		auto snapshot = MakeWorld();
		const FormID root{ 0x3C }, child{ 0x800 }, grandchild{ 0x801 };
		snapshot.worldsByFormID.emplace(child, WorldInfo{ .formID = child, .parentWorld = root, .parentUseFlags = WorldInfo::ParentUseLandData, .defaultWaterHeight = 77.0f });
		snapshot.worldsByFormID.emplace(grandchild, WorldInfo{ .formID = grandchild, .parentWorld = child, .parentUseFlags = WorldInfo::ParentUseLandData | WorldInfo::ParentUseWaterData });
		const WorldTerrain terrain(snapshot, grandchild);
		assert(terrain.LandWorld() == root && terrain.WaterWorld() == child);
		assert(Near(terrain.HeightAt(5, 5), 100));
		assert(Near(terrain.WaterHeight({ 1, 0 }), 77));
		// A missing parent retains the last available world's own defaults.
		snapshot.worldsByFormID.at(child).parentWorld = FormID{ 0x999 };
		snapshot.worldsByFormID.at(child).defaultLandHeight = -45.0f;
		const WorldTerrain missing(snapshot, grandchild);
		assert(missing.LandWorld() == child && Near(missing.HeightAt(5, 5), -45));
		// Cycles terminate; the validator continues to report them as unresolved coverage.
		snapshot.worldsByFormID.at(child).parentWorld = grandchild;
		const WorldTerrain cycle(snapshot, grandchild);
		assert(cycle.LandWorld() == grandchild && cycle.WaterWorld() == child);
	}

	void LodSettingsRoundTripAndAlign()
	{
		LodSettings settings{ .swCell = { -96, -96 }, .stride = 256, .minLevel = 4, .maxLevel = 32 };
		const auto bytes = settings.Serialize();
		const auto parsed = LodSettings::Parse(bytes);
		assert(parsed && parsed->swCell == settings.swCell && parsed->stride == 256 && parsed->maxLevel == 32);
		assert(!LodSettings::Parse(std::span(bytes).first(15)));

		assert(settings.BlockForCell({ -57, 10 }, 4) == (CellCoord{ -60, 8 }));
		assert(settings.BlockForCell({ -57, 10 }, 32) == (CellCoord{ -64, 0 }));
		assert(settings.BlockForCell({ 0, 0 }, 16) == (CellCoord{ 0, 0 }));

		const auto covering = LodSettings::Covering({ -57, -43 }, { 61, 50 });
		assert(covering.swCell == (CellCoord{ -64, -64 }));
		assert(covering.stride == 128);
		assert(covering.Contains({ 61, 50 }) && !covering.Contains({ -65, 0 }));

		const auto quads = QuadsCovering(settings, 8, { -9, 0 }, { 7, 7 });
		assert(quads.size() == 3);
		assert(quads.front() == (QuadKey{ 8, -16, 0 }));
		assert(quads.back() == (QuadKey{ 8, 0, 0 }));
	}

	void PlacementTransformTurnsClockwise()
	{
		// A quarter turn about Z: Bethesda's rotations are clockwise seen from above, so local +X faces world -Y.
		const float position[3]{ 1000.0f, 2000.0f, 3000.0f };
		const float rotation[3]{ 0.0f, 0.0f, std::numbers::pi_v<float> / 2 };
		const auto transform = Transform::FromPlacement(position, rotation, 2.0f);
		const auto moved = transform.Apply({ 1.0f, 0.0f, 0.0f });
		assert(Near(moved.x, 1000.0f) && Near(moved.y, 1998.0f) && Near(moved.z, 3000.0f));
		const auto composed = transform * Transform::FromPlacement(position, rotation, 1.0f);
		const auto twice = composed.Apply({ 0.0f, 0.0f, 0.0f });
		// The inner placement's origin, turned and scaled by the outer one.
		assert(Near(twice.x, 1000.0f + 2.0f * 2000.0f) && Near(twice.y, 2000.0f - 2.0f * 1000.0f));
		assert(FloorDiv(-1, 32) == -1 && FloorDiv(-32, 32) == -1 && FloorDiv(-33, 32) == -2 && FloorDiv(31, 32) == 0);
	}
}

int main()
{
	TerrainSamplesLatticeAndDefaults();
	ParentSourcesFollowIndependentChains();
	LodSettingsRoundTripAndAlign();
	PlacementTransformTurnsClockwise();
	std::cout << "tesworld world tests passed\n";
	return 0;
}