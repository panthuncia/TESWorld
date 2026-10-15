// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Seasons.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>

using namespace tesworld;
using namespace tesworld::seasons;

namespace
{
	constexpr auto kStat = MakeFourCC('S', 'T', 'A', 'T');
	constexpr auto kTree = MakeFourCC('T', 'R', 'E', 'E');

	void NamesAndMaterialIDs()
	{
		assert(SeasonSuffix(Season::Autumn) == "AUT" && SeasonName(Season::Winter) == "Winter");
		assert(SeasonFromSuffix("win") == Season::Winter && !SeasonFromSuffix("WINTER"));
		// RE::MATERIAL_ID: BSCRC32 of the material type's name, in any case.
		assert(MaterialID("Grass") == 1848600814u && MaterialID("dirt") == 3106094762u && MaterialID("SNOW") == 398949039u);
	}

	void SettingsAndSwapFilesParseAsSeasonsReadsThem()
	{
		SeasonsSettings settings;
		ParseSeasonsIni("[Settings]\nSeason Type = 5\n[Winter]\nWorldspaces = Tamriel|DLC2SolstheimWorld\nTrees = false\nTerrain LOD = false\nGrass = 1\n"
			"Ignore auto generated WIN formswap = false\nSkip Land Textures = true\n",
			settings);
		const auto& winter = settings.seasons[0];
		assert(winter.worldspaces.size() == 2 && winter.Covers("tamriel") && !winter.Covers("Riften"));
		assert(!winter.swaps[static_cast<std::size_t>(SwapRecord::Trees)] && winter.swaps[static_cast<std::size_t>(SwapRecord::Statics)]);
		assert(!winter.terrainLod && winter.objectLod && winter.treeLod);
		// SimpleIni reads "1" as true; Grass Cache Helper NG wants "true" itself.
		assert(winter.grass && !winter.grassCaches);
		assert(settings.skipAutomaticWinter[static_cast<std::size_t>(SwapRecord::LandTextures)]);

		SwapFile file;
		ParseSwapFile("\xEF\xBB\xBF[LandTextures]\n0x894~Skyrim.esm|0x89B~Skyrim.esm\n; comment\n[Statics]\nRockMoss|RockSnow\n[Worldspaces]\nBlackreach\n", file);
		assert(file.entries[0].size() == 1 && file.entries[0][0].base == "0x894~Skyrim.esm" && file.entries[0][0].swap == "0x89B~Skyrim.esm");
		assert(file.entries[static_cast<std::size_t>(SwapRecord::Statics)].size() == 1 && file.worldspaces == std::vector<std::string>{ "Blackreach" });

		// Read from Data: Tweaks loads editor IDs, MainFormSwap_WIN.ini is Seasons' own copy, swap files sort by name.
		const std::map<std::string, std::string> data{
			{ "SKSE\\Plugins\\po3_Tweaks.dll", "" },
			{ "Seasons\\Zed_WIN.ini", "[Trees]\nA|B\n" },
			{ "Seasons\\Alpha_WIN.ini", "[Trees]\nA|C\n" },
			{ "Seasons\\Other_SPR.ini", "" },
			{ "Seasons\\MainFormSwap_WIN.ini", "" },
		};
		const DataFiles files{
			.read = [&](std::string_view a_path) -> std::optional<std::string> {
				const auto it = data.find(std::string(a_path));
				return it != data.end() ? std::optional(it->second) : std::nullopt;
			},
			.exists = [&](std::string_view a_path) { return data.contains(std::string(a_path)); },
			.list = [&](std::string_view, std::string_view) {
				std::vector<std::string> paths;
				for (const auto& [path, _] : data) {
					if (path.starts_with("Seasons\\")) paths.push_back(path);
				}
				return paths;
			},
		};
		const auto read = ReadSeasonsSettings(files);
		assert(read.editorIDs && !read.installed && !read.settingsFound && read.automaticWinterFile == "Seasons\\MainFormSwap_WIN.ini");
		assert(read.swapFiles.size() == 3 && read.swapFiles[0].name == "Alpha_WIN.ini" && read.swapFiles[1].season == Season::Spring);
	}

	// Skyrim.esm's snow land textures and a few to swap, two trees and a snowy rock with its plain one.
	StaticWorldSnapshot MakeWorld(std::vector<LoadOrderEntry>& a_loadOrder)
	{
		StaticPluginShard master{};
		master.entry.pluginName = "skyrim.esm";
		master.entry.fileID = FileID{ .slot = 0 };
		master.materialTypes.push_back(MaterialTypeInfo{ .formID = FormID{ 0x10 }, .editorID = "MaterialGrass", .name = "Grass" });
		master.materialTypes.push_back(MaterialTypeInfo{ .formID = FormID{ 0x11 }, .editorID = "MaterialDirt", .name = "Dirt" });
		master.materialTypes.push_back(MaterialTypeInfo{ .formID = FormID{ 0x12 }, .editorID = "MaterialSnow", .name = "Snow" });
		master.grasses.push_back(GrassInfo{ .formID = FormID{ 0x50 } });
		const auto ltex = [&](std::uint32_t a_id, std::string a_editorID, std::uint32_t a_material, std::vector<FormID> a_grass = {}) {
			master.landTextures.push_back(
				LandTextureInfo{ .formID = FormID{ a_id }, .editorID = std::move(a_editorID), .materialFormID = FormID{ a_material }, .grassFormIDs = std::move(a_grass) });
		};
		ltex(0x0000089B, "LSnow01", 0x12);
		ltex(0x0006A1B1, "LSnow02", 0x12);
		ltex(0x00000894, "LGrassSnow01", 0x12);
		ltex(0x0008B01E, "LGrassSnow01NoGrass", 0x12);
		ltex(0x00000C16, "LDirt02", 0x11);
		ltex(0x00000A00, "LFieldGrass", 0x10, { FormID{ 0x50 } });
		ltex(0x00000A01, "LReachGrassNoGrass", 0x10);
		ltex(0x00000A02, "LRiverBed", 0x11);
		ltex(0x00000A03, "LSnowyDirt", 0x11);
		master.baseObjects.push_back(BaseObjectInfo{ .formID = FormID{ 0x100 }, .signature = kTree, .editorID = "TreePine", .modelPath = "Trees\\TreePineForest01.nif" });
		master.baseObjects.push_back(
			BaseObjectInfo{ .formID = FormID{ 0x101 }, .signature = kTree, .editorID = "TreePineSnow", .modelPath = "Trees\\TreePineForestSnow01.nif" });
		master.baseObjects.push_back(BaseObjectInfo{ .formID = FormID{ 0x102 }, .signature = kStat, .editorID = "Rock", .modelPath = "Rocks\\Rock01.nif" });
		master.worlds.push_back(WorldInfo{ .formID = FormID{ 0x3C }, .editorID = "Tamriel" });
		// One cell of LDirt02 with field grass painted over a quadrant.
		master.cells.push_back(CellInfo{ .formID = FormID{ 0x200 }, .worldFormID = FormID{ 0x3C }, .gridX = 0, .gridY = 0 });
		LandInfo land{};
		land.formID = FormID{ 0x201 };
		land.parentCell = FormID{ 0x200 };
		land.hasHeights = true;
		for (std::uint8_t quadrant = 0; quadrant < 4; ++quadrant) {
			land.baseTextures[quadrant] = LandBaseTexture{ .landTextureFormID = FormID{ 0xC16 }, .quadrant = quadrant };
		}
		land.alphaTextures.push_back(LandAlphaTexture{ .landTextureFormID = FormID{ 0xA00 }, .quadrant = 0, .layerIndex = 0 });
		for (std::uint16_t position = 0; position < LandInfo::QuadrantVertexCount; ++position) {
			land.vertexAlphas.push_back(LandVertexAlpha{ .quadrant = 0, .layerIndex = 0, .position = position, .opacity = 1.0f });
		}
		master.lands.push_back(land);

		// A later plugin renaming LDirt02 keeps its place and its first editor ID.
		StaticPluginShard mod{};
		mod.entry.pluginName = "mod.esp";
		mod.entry.fileID = FileID{ .slot = 1 };
		mod.landTextures.push_back(LandTextureInfo{ .formID = FormID{ 0x00000C16 }, .editorID = "Renamed", .materialFormID = FormID{ 0x11 } });
		mod.baseObjects.push_back(BaseObjectInfo{ .formID = FormID{ 0x01000800 }, .signature = kStat, .editorID = "ModRock", .modelPath = "Rocks\\ModRock.nif" });

		a_loadOrder = { master.entry, mod.entry };
		return BuildStaticWorldSnapshot({ master, mod });
	}

	void CreationOrderAndMaterialTypesAreRecorded()
	{
		std::vector<LoadOrderEntry> loadOrder;
		const auto snapshot = MakeWorld(loadOrder);
		assert(snapshot.materialTypesByFormID.at(FormID{ 0x12 }).name == "Snow");
		assert(snapshot.creationOrder.at(FormID{ 0x00000C16 }) < snapshot.creationOrder.at(FormID{ 0x01000800 }));
		assert(snapshot.firstEditorIDs.at(FormID{ 0x00000C16 }) == "LDirt02" && snapshot.landTexturesByFormID.at(FormID{ 0x00000C16 }).editorID == "Renamed");
	}

	void WinterSwapsLandTexturesByMaterial()
	{
		std::vector<LoadOrderEntry> loadOrder;
		const auto snapshot = MakeWorld(loadOrder);
		const auto maps = GenerateWinterSwaps(snapshot, loadOrder, true);
		const auto& land = maps[SwapRecord::LandTextures];
		// Grass with grass to snow grass, without to its no-grass form; LDirt02 to LSnow02; other dirt to LSnow01.
		assert(land.at(FormID{ 0xA00 }) == FormID{ 0x894 } && land.at(FormID{ 0xA01 }) == FormID{ 0x8B01E });
		assert(land.at(FormID{ 0xC16 }) == FormID{ 0x6A1B1 });
		// Editor IDs naming rivers or snow are left, and snow is never swapped.
		assert(!land.contains(FormID{ 0xA02 }) && !land.contains(FormID{ 0xA03 }) && !land.contains(FormID{ 0x89B }));
		assert(maps[SwapRecord::Trees].at(FormID{ 0x100 }) == FormID{ 0x101 });

		// Without Tweaks there are no editor IDs to blacklist by, so the river bed and snowy dirt swap too.
		const auto plain = GenerateWinterSwaps(snapshot, loadOrder, false);
		assert(plain[SwapRecord::LandTextures].at(FormID{ 0xA02 }) == FormID{ 0x89B } && plain[SwapRecord::LandTextures].contains(FormID{ 0xA03 }));
	}

	void ResolvedSwapsApplyPerWorldspace()
	{
		std::vector<LoadOrderEntry> loadOrder;
		const auto snapshot = MakeWorld(loadOrder);
		SeasonsSettings settings{ .editorIDs = true };
		SwapFile file{ .name = "Mine_WIN.ini", .season = Season::Winter };
		file.entries[static_cast<std::size_t>(SwapRecord::LandTextures)] = { { "0xC16~Skyrim.esm", "LSnow01" }, { "0xA00~Skyrim.esm", "0x100~Skyrim.esm" } };
		file.entries[static_cast<std::size_t>(SwapRecord::Statics)] = { { "Rock", "0x800~Mod.esp" }, { "Nothing", "Rock" } };
		file.worldspaces = { "Blackreach" };
		settings.swapFiles.push_back(file);

		const std::array asked{ Season::Winter, Season::Summer };
		const auto resolved = ResolveSeasons(settings, snapshot, loadOrder, asked);
		assert(resolved.seasons.size() == 2 && resolved.seasons[0].season == Season::Winter && resolved.unresolved.size() == 1);
		const auto& winter = resolved.seasons[0];
		// The swap file wins over the automatic swap; a target that is no land texture is dropped in the worldspace.
		assert(winter.maps[SwapRecord::LandTextures].at(FormID{ 0xC16 }) == FormID{ 0x89B });
		assert(winter.config.Covers("Blackreach"));

		const auto tamriel = SwapsIn(winter, snapshot, "Tamriel");
		assert(tamriel.landTextures.at(FormID{ 0xC16 }) == FormID{ 0x89B } && !tamriel.landTextures.contains(FormID{ 0xA00 }));
		assert(tamriel.baseObjects.at(FormID{ 0x102 }) == FormID{ 0x01000800 } && tamriel.baseObjects.at(FormID{ 0x100 }) == FormID{ 0x101 });
		assert(SwapsIn(winter, snapshot, "SomewhereElse").Empty());
		assert(SwapsIn(resolved.seasons[1], snapshot, "Tamriel").Empty());
	}

	void TerrainPaintsTheSwappedLandTextures()
	{
		std::vector<LoadOrderEntry> loadOrder;
		const auto snapshot = MakeWorld(loadOrder);
		const auto layers = [&](const TerrainBuildOptions& a_options) {
			std::vector<FormID> forms;
			for (const auto& layer : BuildTerrainWorld(FormID{ 0x3C }, snapshot, a_options).layers) {
				if (!layer.landTextureFormID.IsNull()) forms.push_back(layer.landTextureFormID);
			}
			std::ranges::sort(forms);
			return forms;
		};
		assert((layers({ .geometry = false }) == std::vector{ FormID{ 0xA00 }, FormID{ 0xC16 } }));

		const auto winter = GenerateWinterSwaps(snapshot, loadOrder, true);
		const std::unordered_map<FormID, FormID, FormIDHash> swaps(winter[SwapRecord::LandTextures].begin(), winter[SwapRecord::LandTextures].end());
		assert((layers({ .geometry = false, .landTextureSwaps = &swaps }) == std::vector{ FormID{ 0x894 }, FormID{ 0x6A1B1 } }));
	}
}

int main()
{
	NamesAndMaterialIDs();
	SettingsAndSwapFilesParseAsSeasonsReadsThem();
	CreationOrderAndMaterialTypesAreRecorded();
	WinterSwapsLandTexturesByMaterial();
	ResolvedSwapsApplyPerWorldspace();
	TerrainPaintsTheSwappedLandTextures();
	std::cout << "seasons tests passed\n";
	return 0;
}
