#pragma once

#include "tesworld/Plugins.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Seasons of Skyrim (powerof3's SeasonsOfSkyrim, 1.9): which forms each season shows in place of others, read as it
// reads them: po3_SeasonsOfSkyrim.ini, the swap files in Data\Seasons, and the winter swaps it generates itself into
// MainFormSwap_WIN.ini. Its LOD and grass caches are named per season ("tamriel.4.0.0.WIN.btr", "<cell>.WIN.cgid").
namespace tesworld::seasons
{
	enum class Season : std::uint8_t
	{
		Winter,
		Spring,
		Summer,
		Autumn
	};
	inline constexpr std::array kSeasons{ Season::Winter, Season::Spring, Season::Summer, Season::Autumn };

	// "Winter", the season's po3_SeasonsOfSkyrim.ini section.
	[[nodiscard]] std::string_view SeasonName(Season a_season);
	// "WIN", as swap files end and seasonal files are named.
	[[nodiscard]] std::string_view SeasonSuffix(Season a_season);
	// The season a suffix names, in any case.
	[[nodiscard]] std::optional<Season> SeasonFromSuffix(std::string_view a_suffix);

	// The sections of a swap file that change what the world shows from afar (VisualEffects never do).
	enum class SwapRecord : std::uint8_t
	{
		LandTextures,
		Activators,
		Furniture,
		MovableStatics,
		Statics,
		Trees,
		Flora
	};
	inline constexpr std::array kSwapRecords{ SwapRecord::LandTextures, SwapRecord::Activators, SwapRecord::Furniture, SwapRecord::MovableStatics,
		SwapRecord::Statics, SwapRecord::Trees, SwapRecord::Flora };
	// The sections that swap base objects: all but land textures.
	inline constexpr std::array kBaseObjectSwapRecords{ SwapRecord::Activators, SwapRecord::Furniture, SwapRecord::MovableStatics, SwapRecord::Statics,
		SwapRecord::Trees, SwapRecord::Flora };

	// "LandTextures", the swap file's section.
	[[nodiscard]] std::string_view SwapRecordSection(SwapRecord a_record);
	// The record type a section swaps: Seasons picks the map by the original form's type.
	[[nodiscard]] FourCC SwapRecordSignature(SwapRecord a_record);

	// Original form -> the form shown instead, ordered so maps compare and print the same every run.
	using SwapMap = std::map<FormID, FormID>;

	struct SwapMaps
	{
		std::array<SwapMap, kSwapRecords.size()> records;

		[[nodiscard]] SwapMap& operator[](SwapRecord a_record) { return records[static_cast<std::size_t>(a_record)]; }
		[[nodiscard]] const SwapMap& operator[](SwapRecord a_record) const { return records[static_cast<std::size_t>(a_record)]; }
		[[nodiscard]] bool Empty() const;

		friend bool operator==(const SwapMaps&, const SwapMaps&) = default;
	};

	// The engine's material ID of a material type (BGSMaterialType::materialID): BSCRC32 of its lower-cased name.
	[[nodiscard]] std::uint32_t MaterialID(std::string_view a_name);

	// One season's po3_SeasonsOfSkyrim.ini section.
	struct SeasonConfig
	{
		std::vector<std::string> worldspaces{ "Tamriel", "MarkarthWorld", "RiftenWorld", "SolitudeWorld", "WhiterunWorld", "DLC1HunterHQWorld",
			"DLC2SolstheimWorld" };
		// Activators, Furniture, Movable Statics, Statics, Trees, Flora, by SwapRecord. Land textures have no switch:
		// a season swaps them wherever it applies.
		std::array<bool, kSwapRecords.size()> swaps{ true, true, true, true, true, true, true };
		// "Grass": seasonal grass types.
		bool grass{ true };
		// Grass Cache Helper NG's reading of the same key: only "true" loads the season's own grass caches.
		bool grassCaches{ true };
		bool objectLod{ true };
		bool terrainLod{ true };
		bool treeLod{ true };

		// The season applies in this worldspace (by editor ID, in any case).
		[[nodiscard]] bool Covers(std::string_view a_worldspace) const;
	};

	// One "base|swap" line of a swap file as written: "0xID~Plugin.esp" or an editor ID on each side.
	struct SwapEntry
	{
		std::string base;
		std::string swap;
	};

	// A Data\Seasons\*_<SUFFIX>.ini file.
	struct SwapFile
	{
		// The file's name, as Seasons sorts them.
		std::string name;
		Season season{ Season::Winter };
		std::array<std::vector<SwapEntry>, kSwapRecords.size()> entries;
		// [Worldspaces]: more worldspaces the season applies in.
		std::vector<std::string> worldspaces;
	};

	struct SeasonsSettings
	{
		// po3_SeasonsOfSkyrim.dll is installed.
		bool installed{ false };
		// GrassCacheHelperNG.dll is installed: the game loads seasonal grass caches only through it.
		bool grassCacheHelper{ false };
		// powerofthree's Tweaks loads editor IDs, which Seasons' editor-ID entries and blacklists need.
		bool editorIDs{ false };
		// po3_SeasonsOfSkyrim.ini was found; otherwise its defaults.
		bool settingsFound{ false };
		// 0 off, 1 to 4 one season all year, 5 seasonal.
		std::uint32_t seasonType{ 5 };
		std::array<SeasonConfig, kSeasons.size()> seasons{};
		// [Winter]: ignore the automatic winter swaps, or some of their sections.
		bool ignoreAutomaticWinter{ false };
		std::array<bool, kSwapRecords.size()> skipAutomaticWinter{};
		// Data\Seasons\MainFormSwap_WIN.ini, Seasons' own copy of the automatic winter swaps; empty when there's none.
		std::string automaticWinterFile;
		// In name order, as Seasons reads them; later entries win.
		std::vector<SwapFile> swapFiles;
	};

	void ParseSeasonsIni(std::string_view a_text, SeasonsSettings& a_settings);
	void ParseSwapFile(std::string_view a_text, SwapFile& a_file);

	// The game's Data folder, as its caller reads it. Paths are relative to Data, with backslashes, in any case.
	struct DataFiles
	{
		// A file's text; nullopt when it isn't there.
		std::function<std::optional<std::string>(std::string_view a_path)> read;
		std::function<bool(std::string_view a_path)> exists;
		// The files directly in a folder with an extension (".ini"), preserving original filename case.
		// Seasons reads Data\Seasons loose; normalized VFS paths lose its case-sensitive matching/sort behavior.
		std::function<std::vector<std::string>(std::string_view a_folder, std::string_view a_extension)> list;
	};

	// Seasons' settings, whether Seasons, Grass Cache Helper NG and Tweaks are installed, and the swap files.
	[[nodiscard]] SeasonsSettings ReadSeasonsSettings(const DataFiles& a_files);

	// The winter swaps Seasons generates into MainFormSwap_WIN.ini when the load order changes
	// (FormSwapMap::GenerateFormSwaps): land textures by material, snowy statics, snow trees, and activators, furniture
	// and movable statics by texture set. Forms are visited in creation order. a_editorIDs: Tweaks loads editor IDs, so
	// Seasons' editor-ID blacklists and snow-shader test can match.
	[[nodiscard]] SwapMaps GenerateWinterSwaps(const StaticWorldSnapshot& a_snapshot, std::span<const LoadOrderEntry> a_loadOrder, bool a_editorIDs);

	// A season resolved against the load order.
	struct ResolvedSeason
	{
		Season season{ Season::Winter };
		// With the swap files' [Worldspaces] added.
		SeasonConfig config;
		SwapMaps maps;
	};

	struct ResolvedSeasons
	{
		// In Seasons' order (WIN, SPR, SUM, AUT).
		std::vector<ResolvedSeason> seasons;
		// The automatic winter swaps on their own, before the swap files; empty unless winter was asked for.
		SwapMaps automaticWinter;
		// The swap file lines that don't resolve: "<file> [<section>] base|swap".
		std::vector<std::string> unresolved;
	};

	// Each of a_seasons' swaps: the automatic winter swaps (unless ignored or skipped), then each swap file in name
	// order, later entries winning. An entry resolves as Seasons' INI::parse_form does: "0xID~Plugin.esp" by its
	// plugin-local ID, or an editor ID, only when Tweaks loads editor IDs.
	[[nodiscard]] ResolvedSeasons ResolveSeasons(const SeasonsSettings& a_settings, const StaticWorldSnapshot& a_snapshot,
		std::span<const LoadOrderEntry> a_loadOrder, std::span<const Season> a_seasons);

	// What a season shows differently in one worldspace.
	struct WorldSwaps
	{
		// Land texture -> the land texture painted instead (both land textures).
		std::unordered_map<FormID, FormID, FormIDHash> landTextures;
		// Base object -> the base object placed instead: the original of its section's type, the replacement a base
		// object, by the season's switches.
		std::unordered_map<FormID, FormID, FormIDHash> baseObjects;

		[[nodiscard]] bool Empty() const { return landTextures.empty() && baseObjects.empty(); }
	};

	// A season's swaps in a worldspace; none where the season doesn't apply.
	[[nodiscard]] WorldSwaps SwapsIn(const ResolvedSeason& a_season, const StaticWorldSnapshot& a_snapshot, std::string_view a_worldspace);
}