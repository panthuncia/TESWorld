// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/TerrainBake.h"
#if defined(TESWORLD_TEST_GPU)
#include "tesworld/TerrainBakeGpu.h"
#endif

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace tesworld;
using namespace tesworld::texture;

namespace
{
	// A patterned texture, so sampling, filtering and stochastic tiling all show.
	void WriteTexture(const std::filesystem::path& a_path, Vec4 a_colour, std::uint32_t a_seed)
	{
		Image image(64, 64);
		std::uint32_t state = a_seed;
		for (std::uint32_t y = 0; y < 64; ++y) {
			for (std::uint32_t x = 0; x < 64; ++x) {
				state = state * 1664525u + 1013904223u;
				const float noise = static_cast<float>(state >> 24) / 255.0f - 0.5f;
				const float wave = 0.15f * std::sin(static_cast<float>(x) * 0.3f) * std::cos(static_cast<float>(y) * 0.2f);
				image.SetTexel(x, y, { a_colour.x + 0.2f * noise + wave, a_colour.y + 0.2f * noise - wave, a_colour.z + 0.1f * noise, 1.0f });
			}
		}
		const auto dds = EncodeDds(BuildMips(std::move(image)), Format::RGBA8);
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream(a_path, std::ios::binary).write(reinterpret_cast<const char*>(dds->data()), static_cast<std::streamsize>(dds->size()));
	}

	// Two cells side by side: rolling heights, tinted vertex colours, grass with rock painted in the first cell's south-west
	// quadrant at a varying strength.
	StaticWorldSnapshot MakeWorld()
	{
		StaticPluginShard shard{};
		shard.entry.pluginName = "test.esm";
		shard.worlds.push_back(WorldInfo{ .formID = FormID{ 0x3C } });
		shard.textureSets.push_back(TextureSetInfo{ .formID = FormID{ 0x10 }, .editorID = "Grass", .texturePaths = { "landscape\\grass.dds", "landscape\\grass_n.dds" } });
		shard.textureSets.push_back(TextureSetInfo{ .formID = FormID{ 0x11 }, .editorID = "Rock", .texturePaths = { "landscape\\rock.dds", "landscape\\rock_n.dds" } });
		shard.landTextures.push_back(LandTextureInfo{ .formID = FormID{ 0x20 }, .textureSetFormID = FormID{ 0x10 } });
		shard.landTextures.push_back(LandTextureInfo{ .formID = FormID{ 0x21 }, .textureSetFormID = FormID{ 0x11 } });
		for (std::int32_t cell = 0; cell < 2; ++cell) {
			shard.cells.push_back(CellInfo{ .formID = FormID{ static_cast<std::uint32_t>(0x100 + cell) }, .worldFormID = FormID{ 0x3C }, .gridX = cell, .gridY = 0 });
			LandInfo land{};
			land.formID = FormID{ static_cast<std::uint32_t>(0x200 + cell) };
			land.parentCell = FormID{ static_cast<std::uint32_t>(0x100 + cell) };
			land.hasHeights = true;
			land.hasVertexColors = true;
			for (std::size_t y = 0; y < LandInfo::VertexSide; ++y) {
				for (std::size_t x = 0; x < LandInfo::VertexSide; ++x) {
					const float gx = static_cast<float>(cell * 32 + static_cast<std::int32_t>(x));
					land.heights[y * LandInfo::VertexSide + x] = 400.0f * std::sin(gx * 0.15f) + 250.0f * std::cos(static_cast<float>(y) * 0.2f);
					land.vertexColors[y * LandInfo::VertexSide + x] = { static_cast<std::uint8_t>(200 + (x * 3) % 55), 230, static_cast<std::uint8_t>(180 + (y * 5) % 75) };
				}
			}
			for (std::uint8_t quadrant = 0; quadrant < 4; ++quadrant) {
				land.baseTextures[quadrant] = LandBaseTexture{ .landTextureFormID = FormID{ 0x20 }, .quadrant = quadrant };
			}
			if (cell == 0) {
				land.alphaTextures.push_back(LandAlphaTexture{ .landTextureFormID = FormID{ 0x21 }, .quadrant = 0, .layerIndex = 0 });
				for (std::uint16_t position = 0; position < LandInfo::QuadrantVertexCount; ++position) {
					const float opacity = static_cast<float>(position % 17) / 16.0f;
					land.vertexAlphas.push_back(LandVertexAlpha{ .quadrant = 0, .layerIndex = 0, .position = position, .opacity = opacity });
				}
			}
			shard.lands.push_back(land);
		}
		return BuildStaticWorldSnapshot({ shard });
	}

	std::vector<std::uint8_t> Bake(TerrainBaker& a_baker, const TerrainBakeRequest& a_request, bool a_normal)
	{
		std::vector<std::uint8_t> bytes;
		a_baker.Bake(a_request, [&](TerrainBakeResult a_result) {
			assert(a_result.Ok());
			bytes = a_normal ? std::move(a_result.normalDds) : std::move(a_result.diffuseDds);
		});
		a_baker.Wait();
		return bytes;
	}

	// The level 0 of an uncompressed DDS, or of a block-compressed one decoded.
	Image Level0(const std::vector<std::uint8_t>& a_dds)
	{
		auto chain = DecodeDds(a_dds);
		assert(chain && !chain->Empty());
		return chain->levels.front();
	}
}

int main()
{
	const auto root = std::filesystem::temp_directory_path() / "tesworld_terrain_bake_test";
	std::filesystem::remove_all(root);
	WriteTexture(root / "textures" / "landscape" / "grass.dds", { 0.2f, 0.6f, 0.2f, 1.0f }, 1);
	WriteTexture(root / "textures" / "landscape" / "rock.dds", { 0.5f, 0.45f, 0.4f, 1.0f }, 2);
	// Normal maps as Skyrim writes them (OpenGL, green up), leaning about.
	WriteTexture(root / "textures" / "landscape" / "grass_n.dds", { 0.6f, 0.5f, 0.9f, 1.0f }, 3);
	WriteTexture(root / "textures" / "landscape" / "rock_n.dds", { 0.4f, 0.6f, 0.85f, 1.0f }, 4);

	const auto snapshot = MakeWorld();
	const WorldTerrain world(snapshot, FormID{ 0x3C });
	const auto terrain = BuildTerrainWorld(FormID{ 0x3C }, snapshot, TerrainBuildOptions{ .geometry = false });
	const vfs::FileSystem files(vfs::FileSystemOptions{ .looseRoots = { root } });
	TerrainTextureCache cache;
	auto layers = LoadTerrainLayers(terrain, files, nullptr, TerrainLayerLoadOptions{ .maxTextureSize = 64, .cache = &cache });
	const TerrainMaterial material(terrain, std::move(layers));
	// The cache gives a second load the same textures.
	const auto again = LoadTerrainLayers(terrain, files, nullptr, TerrainLayerLoadOptions{ .maxTextureSize = 64, .cache = &cache });
	assert(again[1].diffuse == material.Layers()[1].diffuse && again[1].diffuseStochastic == material.Layers()[1].diffuseStochastic);
	const TerrainBakeSettings settings{ .vertexColors = true, .gain = 2.15f, .bias = -0.04f };

	// Both images at once evaluate the material once a texel, and make what each alone makes.
	{
		Image both(48, 48), bothNormal(48, 48), diffuse(48, 48), normal(48, 48);
		BakeTerrainImages(material, world, settings, { 0.0f, 0.0f }, 8192.0f, &both, &bothNormal);
		BakeTerrainImages(material, world, settings, { 0.0f, 0.0f }, 8192.0f, &diffuse, nullptr);
		BakeTerrainImages(material, world, settings, { 0.0f, 0.0f }, 8192.0f, nullptr, &normal);
		assert(both.rgba == diffuse.rgba && bothNormal.rgba == normal.rgba);

		// The CPU baker makes those images, and encodes them.
		auto baker = CreateCpuTerrainBaker(world, settings);
		assert(baker->AddMaterial(material) == 0);
		const TerrainBakeRequest request{ .origin = { 0.0f, 0.0f }, .extent = 8192.0f, .diffuseSize = 48, .normalSize = 48, .diffuseFormat = Format::RGBA8,
			.normalFormat = Format::RGBA8 };
		assert(Level0(Bake(*baker, request, false)).rgba == both.rgba && Level0(Bake(*baker, request, true)).rgba == bothNormal.rgba);
	}

	// Blocks a GPU encoded go back into the DDS EncodeDds would have written.
	{
		Image image(40, 40);
		for (std::uint32_t y = 0; y < 40; ++y) {
			for (std::uint32_t x = 0; x < 40; ++x) image.SetTexel(x, y, { x / 40.0f, y / 40.0f, 0.5f, 1.0f });
		}
		const auto chain = BuildMips(std::move(image));
		const auto dds = EncodeDds(chain, Format::BC7);
		assert(dds);
		const std::size_t header = 4 + 124 + 20;
		const auto rebuilt = EncodeDdsBlocks(40, 40, static_cast<std::uint32_t>(chain.levels.size()), Format::BC7, std::span(*dds).subspan(header));
		assert(rebuilt && *rebuilt == *dds);
	}

#if defined(TESWORLD_TEST_GPU)
	try {
		gpu::Context context;
		assert(context.SelfTest());
		std::cout << "GPU: " << context.Describe() << '\n';
		auto cpu = CreateCpuTerrainBaker(world, settings);
		auto gpu = CreateGpuTerrainBaker(context, world, settings);
		assert(cpu->AddMaterial(material) == 0 && gpu->AddMaterial(material) == 0);

		// Uncompressed, the GPU's images are the CPU's within a step: its divisions and logarithms round differently.
		for (const auto& [origin, extent, size] : { std::tuple{ Vec2{ 0.0f, 0.0f }, 8192.0f, 128u }, std::tuple{ Vec2{ -4096.0f, -4096.0f }, 16384.0f, 64u },
				 std::tuple{ Vec2{ 1024.0f, 512.0f }, 2048.0f, 50u } }) {
			const TerrainBakeRequest request{ .origin = origin, .extent = extent, .diffuseSize = size, .normalSize = size, .diffuseFormat = Format::RGBA8,
				.normalFormat = Format::RGBA8 };
			for (const bool normal : { false, true }) {
				const auto a = Level0(Bake(*cpu, request, normal));
				const auto b = Level0(Bake(*gpu, request, normal));
				assert(a.rgba.size() == b.rgba.size());
				std::size_t differing = 0;
				for (std::size_t i = 0; i < a.rgba.size(); ++i) {
					assert(std::abs(int(a.rgba[i]) - int(b.rgba[i])) <= 1);
					differing += a.rgba[i] != b.rgba[i];
				}
				assert(differing * 1000 <= a.rgba.size());
			}
		}

		// Different sizes bake apart; BC7 from the GPU is as good as DirectXTex's quick mode, and the same twice.
		const TerrainBakeRequest request{ .origin = { 0.0f, 0.0f }, .extent = 8192.0f, .diffuseSize = 128, .normalSize = 64 };
		const auto reference = Level0(Bake(*cpu, TerrainBakeRequest{ request.origin, request.extent, 128, 64, 0, Format::RGBA8, Format::RGBA8 }, false));
		const auto psnr = [&](const Image& a_image) {
			double squared = 0.0;
			for (std::size_t i = 0; i < reference.rgba.size(); ++i) {
				if (i % 4 == 3) continue;
				const double delta = double(reference.rgba[i]) - double(a_image.rgba[i]);
				squared += delta * delta;
			}
			return 10.0 * std::log10(255.0 * 255.0 / (squared / (reference.rgba.size() * 3 / 4)));
		};
		const auto gpuBlocks = Bake(*gpu, request, false);
		const double gpuPsnr = psnr(Level0(gpuBlocks));
		const double cpuPsnr = psnr(Level0(Bake(*cpu, request, false)));
		std::cout << "BC7 PSNR: GPU " << gpuPsnr << " dB, CPU " << cpuPsnr << " dB\n";
		assert(gpuPsnr >= cpuPsnr - 0.5);
		assert(Bake(*gpu, request, false) == gpuBlocks);
		assert(Level0(Bake(*gpu, request, true)).width == 64);
	} catch (const gpu::UnsupportedError& e) {
		std::cout << "no GPU, its tests skipped: " << e.what() << '\n';
	}
#endif

	std::filesystem::remove_all(root);
	std::cout << "tesworld terrain bake tests passed\n";
	return 0;
}
