// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/TerrainMaterial.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace tesworld;
using namespace tesworld::texture;

namespace
{
	bool Near(float a, float b, float tolerance = 0.03f) { return std::abs(a - b) <= tolerance; }

	void WriteTexture(const std::filesystem::path& a_path, Vec4 a_colour)
	{
		Image image(32, 32);
		for (std::uint32_t y = 0; y < 32; ++y) {
			for (std::uint32_t x = 0; x < 32; ++x) image.SetTexel(x, y, a_colour);
		}
		const auto dds = EncodeDds(BuildMips(std::move(image)), Format::RGBA8);
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream(a_path, std::ios::binary).write(reinterpret_cast<const char*>(dds->data()), static_cast<std::streamsize>(dds->size()));
	}

	// One cell: grass everywhere, rock painted at full strength over the south-west quadrant's lattice.
	StaticWorldSnapshot MakeWorld()
	{
		StaticPluginShard shard{};
		shard.entry.pluginName = "test.esm";
		shard.worlds.push_back(WorldInfo{ .formID = FormID{ 0x3C } });
		shard.cells.push_back(CellInfo{ .formID = FormID{ 0x100 }, .worldFormID = FormID{ 0x3C }, .gridX = 0, .gridY = 0 });
		shard.textureSets.push_back(TextureSetInfo{ .formID = FormID{ 0x10 }, .editorID = "Grass", .texturePaths = { "landscape\\grass.dds", "landscape\\grass_n.dds" } });
		shard.textureSets.push_back(TextureSetInfo{ .formID = FormID{ 0x11 }, .editorID = "Rock", .texturePaths = { "landscape\\rock.dds" } });
		shard.landTextures.push_back(LandTextureInfo{ .formID = FormID{ 0x20 }, .textureSetFormID = FormID{ 0x10 } });
		shard.landTextures.push_back(LandTextureInfo{ .formID = FormID{ 0x21 }, .textureSetFormID = FormID{ 0x11 } });

		LandInfo land{};
		land.formID = FormID{ 0x200 };
		land.parentCell = FormID{ 0x100 };
		land.hasHeights = true;
		for (std::uint8_t quadrant = 0; quadrant < 4; ++quadrant) {
			land.baseTextures[quadrant] = LandBaseTexture{ .landTextureFormID = FormID{ 0x20 }, .quadrant = quadrant };
		}
		land.alphaTextures.push_back(LandAlphaTexture{ .landTextureFormID = FormID{ 0x21 }, .quadrant = 0, .layerIndex = 0 });
		for (std::uint16_t position = 0; position < LandInfo::QuadrantVertexCount; ++position) {
			land.vertexAlphas.push_back(LandVertexAlpha{ .quadrant = 0, .layerIndex = 0, .position = position, .opacity = 1.0f });
		}
		shard.lands.push_back(land);
		return BuildStaticWorldSnapshot({ shard });
	}
}

int main()
{
	const auto root = std::filesystem::temp_directory_path() / "tesworld_terrain_material_test";
	std::filesystem::remove_all(root);
	WriteTexture(root / "textures" / "landscape" / "grass.dds", { 0.1f, 0.8f, 0.1f, 1.0f });
	WriteTexture(root / "textures" / "landscape" / "rock.dds", { 0.5f, 0.5f, 0.5f, 1.0f });
	// A normal map leaning east, written as Skyrim writes them (OpenGL, green up): flat in y.
	WriteTexture(root / "textures" / "landscape" / "grass_n.dds", { 0.75f, 0.5f, 0.933f, 1.0f });

	const auto snapshot = MakeWorld();
	const auto terrain = BuildTerrainWorld(FormID{ 0x3C }, snapshot, TerrainBuildOptions{ .geometry = false });
	assert(terrain.vertices.empty() && !terrain.regions.empty());

	const vfs::FileSystem files(vfs::FileSystemOptions{ .looseRoots = { root } });
	std::vector<std::string> warnings;
	for (const bool stochastic : { false, true }) {
		auto layers = LoadTerrainLayers(terrain, files, nullptr, TerrainLayerLoadOptions{ .maxTextureSize = 32, .stochastic = stochastic, .gaussian = stochastic }, &warnings);
		assert(layers.size() == terrain.layers.size());
		const TerrainMaterial material(terrain, std::move(layers), TerrainMaterialSettings{ .stochastic = stochastic });

		// The north-east quadrant is grass only.
		const auto grass = material.Evaluate({ 3500.0f, 3500.0f }, 64.0f);
		assert(grass.valid);
		assert(Near(grass.albedo.x, 0.1f, 0.06f) && Near(grass.albedo.y, 0.8f, 0.06f));
		// The normal map leans east (x > 0) and is flat north-south.
		assert(grass.normal.x > 0.3f && Near(grass.normal.y, 0.0f, 0.08f));
		// Deep in the painted quadrant: rock.
		const auto rock = material.Evaluate({ 900.0f, 900.0f }, 64.0f);
		assert(Near(rock.albedo.x, 0.5f, 0.06f) && Near(rock.albedo.y, 0.5f, 0.06f));
		// Outside the land: nothing.
		assert(!material.Evaluate({ -5000.0f, 0.0f }, 64.0f).valid);
	}

	// A flat terrain-space normal on sloping ground follows the ground.
	const auto turned = FromTerrainTangentSpace({ 0, 0, 1 }, Normalize({ 1, 0, 1 }));
	assert(Near(turned.x, 0.7071f) && Near(turned.z, 0.7071f));
	const auto east = FromTerrainTangentSpace({ 1, 0, 0 }, { 0, 0, 1 });
	assert(Near(east.x, 1.0f));
	const auto north = FromTerrainTangentSpace({ 0, 1, 0 }, { 0, 0, 1 });
	assert(Near(north.y, 1.0f));

	std::filesystem::remove_all(root);
	std::cout << "tesworld terrain material tests passed\n";
	return 0;
}
