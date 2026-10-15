#include "tesworld/Seasons.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace tesworld::seasons
{
	namespace
	{
		constexpr auto kStat = MakeFourCC('S', 'T', 'A', 'T');
		constexpr auto kTree = MakeFourCC('T', 'R', 'E', 'E');
		constexpr auto kActi = MakeFourCC('A', 'C', 'T', 'I');
		constexpr auto kFurn = MakeFourCC('F', 'U', 'R', 'N');
		constexpr auto kMstt = MakeFourCC('M', 'S', 'T', 'T');
		constexpr auto kFlor = MakeFourCC('F', 'L', 'O', 'R');
		constexpr auto kLtex = MakeFourCC('L', 'T', 'E', 'X');
		constexpr auto kMods = MakeFourCC('M', 'O', 'D', 'S');

		// RE::MATERIAL_ID values the land-texture rule switches on.
		constexpr std::uint32_t kMaterialGrass = 1848600814u;
		constexpr std::uint32_t kMaterialDirt = 3106094762u;
		constexpr std::uint32_t kMaterialStone = 3741512247u;
		constexpr std::uint32_t kMaterialStoneBroken = 131151687u;
		constexpr std::uint32_t kMaterialGravel = 428587608u;
		constexpr std::uint32_t kMaterialSnow = 398949039u;
		constexpr std::uint32_t kMaterialIce = 873356572u;
		constexpr std::uint32_t kMaterialSand = 2168343821u;
		constexpr std::uint32_t kMaterialMud = 1486385281u;

		std::size_t Index(SwapRecord a_record) { return static_cast<std::size_t>(a_record); }

		std::string_view TrimView(std::string_view a_text)
		{
			while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.front()))) a_text.remove_prefix(1);
			while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.back()))) a_text.remove_suffix(1);
			return a_text;
		}

		bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
		{
			return a_lhs.size() == a_rhs.size() &&
			       std::ranges::equal(a_lhs, a_rhs, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
		}

		// REX::STR::ICONTAINS: an empty needle never matches.
		bool IContains(std::string_view a_haystack, std::string_view a_needle)
		{
			if (a_needle.empty() || a_needle.size() > a_haystack.size()) return false;
			return !std::ranges::search(a_haystack, a_needle, [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); }).empty();
		}

		bool IEndsWith(std::string_view a_text, std::string_view a_suffix)
		{
			return a_text.size() >= a_suffix.size() && IEquals(a_text.substr(a_text.size() - a_suffix.size()), a_suffix);
		}

		// The name and stem of a path with either slash.
		std::string_view FileName(std::string_view a_path)
		{
			const auto slash = a_path.find_last_of("\\/");
			return slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);
		}

		std::string_view Stem(std::string_view a_path)
		{
			const auto name = FileName(a_path);
			const auto dot = name.rfind('.');
			return dot == std::string_view::npos ? name : name.substr(0, dot);
		}

		// An INI's lines by section, as SimpleIni keeps them: "key" -> "value", keys only too.
		using Ini = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;

		Ini ParseIni(std::string_view a_text)
		{
			Ini ini;
			while (!a_text.empty()) {
				const auto end = a_text.find('\n');
				auto line = TrimView(a_text.substr(0, end));
				a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
				if (line.starts_with("\xEF\xBB\xBF")) line = TrimView(line.substr(3));
				if (line.empty() || line.front() == ';' || line.front() == '#') continue;
				if (line.front() == '[') {
					const auto close = line.find(']');
					ini.emplace_back(std::string(TrimView(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1))),
						std::vector<std::pair<std::string, std::string>>{});
					continue;
				}
				if (ini.empty()) ini.emplace_back(std::string(), std::vector<std::pair<std::string, std::string>>{});
				const auto equals = line.find('=');
				ini.back().second.emplace_back(std::string(TrimView(line.substr(0, equals))),
					equals == std::string_view::npos ? std::string() : std::string(TrimView(line.substr(equals + 1))));
			}
			return ini;
		}

		// The last value of a key, as SimpleIni's GetValue with MultiKey returns.
		const std::string* IniGet(const Ini& a_ini, std::string_view a_section, std::string_view a_key)
		{
			const std::string* found = nullptr;
			for (const auto& [section, keys] : a_ini) {
				if (!IEquals(section, a_section)) continue;
				for (const auto& [key, value] : keys) {
					if (IEquals(key, a_key)) found = &value;
				}
			}
			return found;
		}

		// SimpleIni's GetBoolValue: the first letter decides; anything else keeps the default.
		bool IniBool(const Ini& a_ini, std::string_view a_section, std::string_view a_key, bool a_default)
		{
			const auto* value = IniGet(a_ini, a_section, a_key);
			if (!value || value->empty()) return a_default;
			switch ((*value)[0]) {
			case 't': case 'T': case 'y': case 'Y': case '1': return true;
			case 'f': case 'F': case 'n': case 'N': case '0': return false;
			case 'o': case 'O':
				if (value->size() > 1 && ((*value)[1] == 'n' || (*value)[1] == 'N')) return true;
				if (value->size() > 1 && ((*value)[1] == 'f' || (*value)[1] == 'F')) return false;
				return a_default;
			default: return a_default;
			}
		}

		std::vector<std::string> SplitWorldspaces(std::string_view a_text)
		{
			std::vector<std::string> names;
			while (!a_text.empty()) {
				const auto end = a_text.find('|');
				if (const auto name = TrimView(a_text.substr(0, end)); !name.empty()) names.emplace_back(name);
				a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
			}
			return names;
		}

		std::uint32_t CreationOrder(const StaticWorldSnapshot& a_snapshot, FormID a_form)
		{
			const auto it = a_snapshot.creationOrder.find(a_form);
			return it != a_snapshot.creationOrder.end() ? it->second : 0xFFFFFFFFu;
		}

		// INI::parse_form: "0xID~Plugin.esp" (hex, plugin-local) or an editor ID, which resolves only when Tweaks loads
		// editor IDs; the first form created with it wins.
		class FormResolver
		{
		public:
			FormResolver(const StaticWorldSnapshot& a_snapshot, std::span<const LoadOrderEntry> a_loadOrder, bool a_editorIDs) :
				_loadOrder(a_loadOrder)
			{
				if (!a_editorIDs) return;
				std::vector<std::pair<std::uint32_t, FormID>> ordered;
				ordered.reserve(a_snapshot.firstEditorIDs.size());
				for (const auto& [form, _] : a_snapshot.firstEditorIDs) ordered.emplace_back(CreationOrder(a_snapshot, form), form);
				std::ranges::sort(ordered);
				for (const auto& [_, form] : ordered) _byEditorID.try_emplace(LowerAscii(a_snapshot.firstEditorIDs.at(form)), form);
			}

			std::optional<FormID> Resolve(std::string_view a_text) const
			{
				const auto text = TrimView(a_text);
				if (const auto tilde = text.find('~'); tilde != std::string_view::npos) {
					if (text.find('~', tilde + 1) != std::string_view::npos) return std::nullopt;
					const std::string id(TrimView(text.substr(0, tilde)));
					const auto plugin = LowerAscii(TrimView(text.substr(tilde + 1)));
					char* end = nullptr;
					const auto local = static_cast<std::uint32_t>(std::strtoul(id.c_str(), &end, 16));
					if (end == id.c_str()) return std::nullopt;
					const auto it = std::ranges::find_if(_loadOrder, [&](const LoadOrderEntry& a_entry) { return LowerAscii(a_entry.pluginName) == plugin; });
					if (it == _loadOrder.end()) return std::nullopt;
					const auto objectID = it->fileID.kind == ModuleKind::Light ? (local & 0xFFFu) : (local & 0xFFFFFFu);
					return FormID{ it->fileID.BaseFormID() | objectID };
				}
				if (const auto it = _byEditorID.find(LowerAscii(text)); it != _byEditorID.end()) return it->second;
				return std::nullopt;
			}

		private:
			std::span<const LoadOrderEntry> _loadOrder;
			std::unordered_map<std::string, FormID> _byEditorID;
		};

		// model::process_model_path: from the last backslash (kept), lower-cased.
		std::string ProcessModelPath(std::string_view a_model)
		{
			if (const auto pos = a_model.rfind('\\'); pos != std::string_view::npos) a_model.remove_prefix(pos);
			return LowerAscii(a_model);
		}

		// Seasons' FormSwapMap::GenerateFormSwaps for winter.
		class WinterGenerator
		{
		public:
			WinterGenerator(const StaticWorldSnapshot& a_snapshot, std::span<const LoadOrderEntry> a_loadOrder, bool a_editorIDs) :
				_snapshot(a_snapshot),
				_editorIDs(a_editorIDs)
			{
				for (const auto& [_, base] : a_snapshot.baseObjectsByFormID) _bySignature[base.signature].push_back(&base);
				for (auto& [_, forms] : _bySignature) {
					std::ranges::sort(forms, {}, [&](const BaseObjectInfo* a_form) { return std::pair(CreationOrder(_snapshot, a_form->formID), a_form->formID); });
				}
				for (const auto& [_, landTexture] : a_snapshot.landTexturesByFormID) _landTextures.push_back(&landTexture);
				std::ranges::sort(_landTextures, {}, [&](const LandTextureInfo* a_ltex) { return std::pair(CreationOrder(_snapshot, a_ltex->formID), a_ltex->formID); });
				for (const auto& entry : a_loadOrder) {
					if (LowerAscii(entry.pluginName) == "snowoverskyrim.esp") _snowOverSkyrim = entry.fileID;
				}
				if (a_editorIDs) {
					for (const auto& [formID, _] : a_snapshot.materialObjectsByFormID) {
						if (IContains(EditorID(formID), "Snow")) _snowShaders.push_back(formID);
					}
				}
			}

			SwapMap LandTextures() const
			{
				SwapMap map;
				for (const auto* landTexture : _landTextures) {
					if (const auto snow = LandTextureSnowVariant(*landTexture)) map.emplace(landTexture->formID, *snow);
				}
				return map;
			}

			SwapMap Statics() const
			{
				constexpr std::array<std::string_view, 3> snowBlacklist{ "Ice", "Icicle", "Frozen" };
				constexpr std::array<std::string_view, 7> blacklist{ "Ice", "Icicle", "Frozen", "LoadScreen", "INTERIOR", "INV", "DynDOLOD" };
				const auto inBlacklist = [&](const BaseObjectInfo& a_stat, std::span<const std::string_view> a_list) {
					const auto editorID = EditorID(a_stat.formID);
					return std::ranges::any_of(a_list, [&](std::string_view a_word) { return IContains(editorID, a_word); });
				};
				const auto& statics = Forms(kStat);
				std::map<std::string, const BaseObjectInfo*> snowStatics;
				if (_snowOverSkyrim) {
					for (const auto* stat : statics) {
						if (IsFormInMod(*_snowOverSkyrim, stat->formID) && !stat->modelPath.empty()) snowStatics.emplace(ProcessModelPath(stat->modelPath), stat);
					}
				}
				for (const auto* stat : statics) {
					if ((HasSnowShader(*stat) && OnlyContainsTextureSet(*stat, "Snow", "Mask")) || MustOnlyContainTextureSet(*stat, "Snow", "Mask")) {
						if (stat->modelPath.empty() || inBlacklist(*stat, snowBlacklist)) continue;
						snowStatics.emplace(ProcessModelPath(stat->modelPath), stat);
					}
				}
				SwapMap map;
				for (const auto& [snowPath, snowStat] : snowStatics) {
					for (const auto* stat : statics) {
						std::string path = stat->modelPath;
						// REX::STR::REPLACE_LAST_INSTANCE: case-sensitive.
						if (const auto pos = path.rfind("Moss"); pos != std::string::npos) path.erase(pos, 4);
						if (IContains(path, snowPath) && snowStat != stat && !HasSnowShader(*stat) && !inBlacklist(*stat, blacklist)) {
							map.emplace(stat->formID, snowStat->formID);
						}
					}
				}
				return map;
			}

			SwapMap Trees() const
			{
				const auto& trees = Forms(kTree);
				std::map<std::string, const BaseObjectInfo*> snowTrees;
				for (const auto* tree : trees) {
					if (std::string path = tree->modelPath; IContains(path, "Snow")) {
						// REX::STR::REPLACE_ALL: case-sensitive.
						for (auto pos = path.find("Snow"); pos != std::string::npos; pos = path.find("Snow", pos)) path.erase(pos, 4);
						snowTrees.emplace(path, tree);
					}
				}
				SwapMap map;
				for (const auto& [path, snowTree] : snowTrees) {
					for (const auto* tree : trees) {
						if (IContains(tree->modelPath, path) && tree != snowTree) map.emplace(tree->formID, snowTree->formID);
					}
				}
				return map;
			}

			// get_snow_variants_by_form: activators, furniture and movable statics.
			SwapMap ByTextureSwap(FourCC a_signature) const
			{
				constexpr std::array<std::string_view, 3> blacklist{ "Blacksmith", "Frozen", "Marker" };
				const auto& forms = Forms(a_signature);
				std::map<std::string, const BaseObjectInfo*> snowForms;
				for (const auto* form : forms) {
					if (OnlyContainsTextureSet(*form, "Snow") && !form->modelPath.empty()) snowForms.emplace(ProcessModelPath(form->modelPath), form);
				}
				SwapMap map;
				for (const auto& [path, snowForm] : snowForms) {
					for (const auto* form : forms) {
						if (IContains(form->modelPath, path) && !ContainsTextureSet(*form, "Snow") && !ContainsTextureSet(*form, "Frozen") &&
							!std::ranges::any_of(blacklist, [&](std::string_view a_word) { return IContains(form->modelPath, a_word); })) {
							map.emplace(form->formID, snowForm->formID);
						}
					}
				}
				return map;
			}

		private:
			// clib_util::editorID::get_editorID: for these form types only Tweaks keeps one (the first definition's).
			std::string_view EditorID(FormID a_form) const
			{
				if (!_editorIDs) return {};
				const auto it = _snapshot.firstEditorIDs.find(a_form);
				return it != _snapshot.firstEditorIDs.end() ? std::string_view(it->second) : std::string_view{};
			}

			const std::vector<const BaseObjectInfo*>& Forms(FourCC a_signature) const
			{
				static const std::vector<const BaseObjectInfo*> none;
				const auto it = _bySignature.find(a_signature);
				return it != _bySignature.end() ? it->second : none;
			}

			// TESFile::IsFormInMod: by FormID, so forms the plugin only overrides don't count.
			static bool IsFormInMod(FileID a_file, FormID a_form)
			{
				if (a_file.kind == ModuleKind::Light) return (a_form.value & 0xFFFFF000u) == a_file.BaseFormID();
				return (a_form.value >> 24) == a_file.slot;
			}

			// Cache::is_snow_shader: a material object whose editor ID contains "Snow".
			bool HasSnowShader(const BaseObjectInfo& a_form) const
			{
				return !a_form.material.IsNull() && std::ranges::find(_snowShaders, a_form.material) != _snowShaders.end();
			}

			// The diffuse texture of each of the model's alternate texture sets; null where a set doesn't resolve.
			std::vector<const std::string*> Diffuses(const BaseObjectInfo& a_form) const
			{
				std::vector<const std::string*> diffuses;
				for (const auto& alternate : a_form.alternateTextures) {
					if (alternate.subrecordSignature != kMods) continue;
					const auto it = _snapshot.textureSetsByFormID.find(alternate.textureSetFormID);
					diffuses.push_back(it != _snapshot.textureSetsByFormID.end() ? &it->second.texturePaths[0] : nullptr);
				}
				return diffuses;
			}

			// model::contains_textureset.
			bool ContainsTextureSet(const BaseObjectInfo& a_form, std::string_view a_word) const
			{
				return std::ranges::any_of(Diffuses(a_form), [&](const std::string* a_diffuse) { return a_diffuse && IContains(*a_diffuse, a_word); });
			}

			// model::only_contains_textureset(form, word): false without alternate textures.
			bool OnlyContainsTextureSet(const BaseObjectInfo& a_form, std::string_view a_word) const
			{
				const auto diffuses = Diffuses(a_form);
				return !diffuses.empty() && std::ranges::all_of(diffuses, [&](const std::string* a_diffuse) { return a_diffuse && IContains(*a_diffuse, a_word); });
			}

			static bool AllContainEither(const std::vector<const std::string*>& a_diffuses, std::string_view a_first, std::string_view a_second)
			{
				return std::ranges::all_of(a_diffuses, [&](const std::string* a_diffuse) {
					return a_diffuse && (IContains(*a_diffuse, a_first) || IContains(*a_diffuse, a_second));
				});
			}

			// model::only_contains_textureset(form, {first, second}): true without alternate textures.
			bool OnlyContainsTextureSet(const BaseObjectInfo& a_form, std::string_view a_first, std::string_view a_second) const
			{
				const auto diffuses = Diffuses(a_form);
				return diffuses.empty() || AllContainEither(diffuses, a_first, a_second);
			}

			// model::must_only_contain_textureset: false without alternate textures.
			bool MustOnlyContainTextureSet(const BaseObjectInfo& a_form, std::string_view a_first, std::string_view a_second) const
			{
				const auto diffuses = Diffuses(a_form);
				return !diffuses.empty() && AllContainEither(diffuses, a_first, a_second);
			}

			bool HasGrass(const LandTextureInfo& a_landTexture) const
			{
				return std::ranges::any_of(a_landTexture.grassFormIDs, [&](FormID a_grass) { return _snapshot.grassesByFormID.contains(a_grass); });
			}

			// FormSwapMap::GenerateLandTextureSnowVariant.
			std::optional<FormID> LandTextureSnowVariant(const LandTextureInfo& a_landTexture) const
			{
				constexpr std::array<std::string_view, 6> blacklist{ "Snow", "Ice", "Winter", "Frozen", "Coast", "River" };
				const auto editorID = EditorID(a_landTexture.formID);
				if (std::ranges::any_of(blacklist, [&](std::string_view a_word) { return IContains(editorID, a_word); })) return std::nullopt;

				constexpr std::uint32_t LSnow01 = 0x0000089B;
				constexpr std::uint32_t LSnow02 = 0x0006A1B1;
				std::uint32_t material = 0;
				if (const auto it = _snapshot.materialTypesByFormID.find(a_landTexture.materialFormID); it != _snapshot.materialTypesByFormID.end()) {
					material = MaterialID(it->second.name);
				}

				std::uint32_t formID = 0;
				switch (material) {
				case kMaterialGrass:
					switch (a_landTexture.formID.value) {
					case 0x0001342A:  // LFieldGrass02
					case 0x00024E46:  // LFieldGrass01NoGrass
					case 0x00024E30:  // LTundra01
					case 0x000A2741:  // LTundra01NoGrass
						formID = LSnow01;
						break;
					case 0x000134B7:  // LFieldDirtGrass01
					case 0x000300E4:  // LTundra02
						formID = LSnow02;
						break;
					default:
						formID = HasGrass(a_landTexture) ? 0x00000894u /* LGrassSnow01 */ : 0x0008B01Eu /* LGrassSnow01NoGrass */;
						break;
					}
					break;
				case kMaterialDirt:
					switch (a_landTexture.formID.value) {
					case 0x00000C16:  // LDirt02
						formID = LSnow02;
						break;
					case 0x000B424C:  // LDirtPath01
						formID = 0x0001B082u;  // LDirtSnowPath01
						break;
					default:
						formID = LSnow01;
						break;
					}
					break;
				case kMaterialStone:
				case kMaterialStoneBroken:
				case kMaterialGravel:
					switch (a_landTexture.formID.value) {
					case 0x0002C6C6:  // LTundraRocks01
						formID = 0x0006A1AFu;  // LSnowRocks01
						break;
					case 0x0006DE8B:  // LTundraRocks01NoRocks
						formID = LSnow01;
						break;
					default:
						formID = HasGrass(a_landTexture) ? 0x000F871Fu /* LSnowRockswGrass */ : 0x0006A1AFu /* LSnowRocks01 */;
						break;
					}
					break;
				case kMaterialSnow:
				case kMaterialIce:
				case kMaterialSand:
				case kMaterialMud:
					return std::nullopt;
				default:
					formID = LSnow02;
					break;
				}
				// LookupByID<TESLandTexture>: the target must exist.
				if (!_snapshot.landTexturesByFormID.contains(FormID{ formID })) return std::nullopt;
				return FormID{ formID };
			}

			const StaticWorldSnapshot& _snapshot;
			bool _editorIDs{ false };
			std::map<FourCC, std::vector<const BaseObjectInfo*>> _bySignature;
			std::vector<const LandTextureInfo*> _landTextures;
			std::optional<FileID> _snowOverSkyrim;
			std::vector<FormID> _snowShaders;
		};
	}

	std::string_view SeasonName(Season a_season)
	{
		constexpr std::array<std::string_view, kSeasons.size()> names{ "Winter", "Spring", "Summer", "Autumn" };
		return names[static_cast<std::size_t>(a_season)];
	}

	std::string_view SeasonSuffix(Season a_season)
	{
		constexpr std::array<std::string_view, kSeasons.size()> suffixes{ "WIN", "SPR", "SUM", "AUT" };
		return suffixes[static_cast<std::size_t>(a_season)];
	}

	std::optional<Season> SeasonFromSuffix(std::string_view a_suffix)
	{
		for (const auto season : kSeasons) {
			if (IEquals(SeasonSuffix(season), a_suffix)) return season;
		}
		return std::nullopt;
	}

	std::string_view SwapRecordSection(SwapRecord a_record)
	{
		constexpr std::array<std::string_view, kSwapRecords.size()> sections{ "LandTextures", "Activators", "Furniture", "MovableStatics", "Statics", "Trees",
			"Flora" };
		return sections[Index(a_record)];
	}

	FourCC SwapRecordSignature(SwapRecord a_record)
	{
		constexpr std::array<FourCC, kSwapRecords.size()> signatures{ kLtex, kActi, kFurn, kMstt, kStat, kTree, kFlor };
		return signatures[Index(a_record)];
	}

	bool SwapMaps::Empty() const
	{
		return std::ranges::all_of(records, [](const SwapMap& a_map) { return a_map.empty(); });
	}

	std::uint32_t MaterialID(std::string_view a_name)
	{
		static const auto table = [] {
			std::array<std::uint32_t, 256> result{};
			for (std::uint32_t i = 0; i < 256; ++i) {
				auto value = i;
				for (int bit = 0; bit < 8; ++bit) value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
				result[i] = value;
			}
			return result;
		}();
		std::uint32_t crc = 0;
		for (const unsigned char ch : a_name) crc = table[(crc ^ static_cast<unsigned char>(std::tolower(ch))) & 0xFFu] ^ (crc >> 8);
		return crc;
	}

	bool SeasonConfig::Covers(std::string_view a_worldspace) const
	{
		return std::ranges::any_of(worldspaces, [&](const std::string& a_name) { return IEquals(a_name, a_worldspace); });
	}

	void ParseSeasonsIni(std::string_view a_text, SeasonsSettings& a_settings)
	{
		const auto ini = ParseIni(a_text);
		if (const auto* type = IniGet(ini, "Settings", "Season Type")) {
			char* end = nullptr;
			const auto value = std::strtol(type->c_str(), &end, 10);
			if (end != type->c_str()) a_settings.seasonType = static_cast<std::uint32_t>(value);
		}
		for (const auto season : kSeasons) {
			const auto section = SeasonName(season);
			auto& config = a_settings.seasons[static_cast<std::size_t>(season)];
			if (const auto* worldspaces = IniGet(ini, section, "Worldspaces")) config.worldspaces = SplitWorldspaces(*worldspaces);
			config.swaps[Index(SwapRecord::Activators)] = IniBool(ini, section, "Activators", true);
			config.swaps[Index(SwapRecord::Furniture)] = IniBool(ini, section, "Furniture", true);
			config.swaps[Index(SwapRecord::MovableStatics)] = IniBool(ini, section, "Movable Statics", true);
			config.swaps[Index(SwapRecord::Statics)] = IniBool(ini, section, "Statics", true);
			config.swaps[Index(SwapRecord::Trees)] = IniBool(ini, section, "Trees", true);
			config.swaps[Index(SwapRecord::Flora)] = IniBool(ini, section, "Flora", true);
			config.grass = IniBool(ini, section, "Grass", true);
			if (const auto* grass = IniGet(ini, section, "Grass")) config.grassCaches = TrimView(*grass) == "true";
			config.objectLod = IniBool(ini, section, "Object LOD", true);
			config.terrainLod = IniBool(ini, section, "Terrain LOD", true);
			config.treeLod = IniBool(ini, section, "Tree LOD", true);
		}
		a_settings.ignoreAutomaticWinter = IniBool(ini, "Winter", "Ignore auto generated WIN formswap", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::LandTextures)] = IniBool(ini, "Winter", "Skip Land Textures", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Activators)] = IniBool(ini, "Winter", "Skip Activator", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Furniture)] = IniBool(ini, "Winter", "Skip Furniture", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::MovableStatics)] = IniBool(ini, "Winter", "Skip Movable Statics", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Statics)] = IniBool(ini, "Winter", "Skip Statics", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Trees)] = IniBool(ini, "Winter", "Skip Tree", false);
	}

	void ParseSwapFile(std::string_view a_text, SwapFile& a_file)
	{
		// Keys only, one "base|swap" a line, as Seasons' SimpleIni (AllowKeyOnly, MultiKey) reads them.
		for (const auto& [section, keys] : ParseIni(a_text)) {
			const bool worldspaces = IEquals(section, "Worldspaces");
			std::optional<std::size_t> record;
			for (const auto candidate : kSwapRecords) {
				if (IEquals(section, SwapRecordSection(candidate))) record = Index(candidate);
			}
			for (const auto& [key, value] : keys) {
				const std::string_view line = key.empty() ? std::string_view(value) : std::string_view(key);
				if (line.empty()) continue;
				if (worldspaces) {
					a_file.worldspaces.emplace_back(line);
				} else if (record) {
					const auto bar = line.find('|');
					if (bar == std::string_view::npos) continue;
					const auto rest = line.substr(bar + 1);
					a_file.entries[*record].push_back(SwapEntry{ .base = std::string(line.substr(0, bar)), .swap = std::string(rest.substr(0, rest.find('|'))) });
				}
			}
		}
	}

	SeasonsSettings ReadSeasonsSettings(const DataFiles& a_files)
	{
		SeasonsSettings settings;
		settings.installed = a_files.exists("SKSE\\Plugins\\po3_SeasonsOfSkyrim.dll");
		settings.grassCacheHelper = a_files.exists("SKSE\\Plugins\\GrassCacheHelperNG.dll");
		if (a_files.exists("SKSE\\Plugins\\po3_Tweaks.dll")) {
			settings.editorIDs = true;
			if (const auto text = a_files.read("SKSE\\Plugins\\po3_Tweaks.ini")) settings.editorIDs = IniBool(ParseIni(*text), "Fixes", "Load EditorIDs", true);
		}
		if (const auto text = a_files.read("SKSE\\Plugins\\po3_SeasonsOfSkyrim.ini")) {
			settings.settingsFound = true;
			ParseSeasonsIni(*text, settings);
		}
		for (const auto& path : a_files.list("Seasons", ".ini")) {
			const auto stem = Stem(path);
			if (IContains(stem, "MainFormSwap")) {
				if (IEquals(stem, "MainFormSwap_WIN")) settings.automaticWinterFile = path;
				continue;
			}
			for (const auto season : kSeasons) {
				if (!IEndsWith(stem, SeasonSuffix(season))) continue;
				SwapFile file{ .name = std::string(FileName(path)), .season = season };
				if (const auto text = a_files.read(path)) ParseSwapFile(*text, file);
				settings.swapFiles.push_back(std::move(file));
			}
		}
		// Seasons sorts each season's files by path.
		std::ranges::sort(settings.swapFiles, {}, &SwapFile::name);
		return settings;
	}

	SwapMaps GenerateWinterSwaps(const StaticWorldSnapshot& a_snapshot, std::span<const LoadOrderEntry> a_loadOrder, bool a_editorIDs)
	{
		const WinterGenerator generator(a_snapshot, a_loadOrder, a_editorIDs);
		SwapMaps maps;
		maps[SwapRecord::LandTextures] = generator.LandTextures();
		maps[SwapRecord::Activators] = generator.ByTextureSwap(kActi);
		maps[SwapRecord::Furniture] = generator.ByTextureSwap(kFurn);
		maps[SwapRecord::MovableStatics] = generator.ByTextureSwap(kMstt);
		maps[SwapRecord::Statics] = generator.Statics();
		maps[SwapRecord::Trees] = generator.Trees();
		return maps;
	}

	ResolvedSeasons ResolveSeasons(const SeasonsSettings& a_settings, const StaticWorldSnapshot& a_snapshot, std::span<const LoadOrderEntry> a_loadOrder,
		std::span<const Season> a_seasons)
	{
		ResolvedSeasons resolved;
		const FormResolver resolver(a_snapshot, a_loadOrder, a_settings.editorIDs);
		for (const auto season : kSeasons) {
			if (std::ranges::find(a_seasons, season) == a_seasons.end()) continue;
			ResolvedSeason entry{ .season = season, .config = a_settings.seasons[static_cast<std::size_t>(season)] };
			if (season == Season::Winter) {
				resolved.automaticWinter = GenerateWinterSwaps(a_snapshot, a_loadOrder, a_settings.editorIDs);
				if (!a_settings.ignoreAutomaticWinter) {
					for (const auto record : kSwapRecords) {
						if (!a_settings.skipAutomaticWinter[Index(record)]) entry.maps[record] = resolved.automaticWinter[record];
					}
				}
			}
			for (const auto& file : a_settings.swapFiles) {
				if (file.season != season) continue;
				for (const auto record : kSwapRecords) {
					for (const auto& [from, to] : file.entries[Index(record)]) {
						const auto base = resolver.Resolve(from);
						const auto swap = resolver.Resolve(to);
						if (base && swap) {
							entry.maps[record].insert_or_assign(*base, *swap);
						} else {
							resolved.unresolved.push_back(file.name + " [" + std::string(SwapRecordSection(record)) + "] " + from + "|" + to);
						}
					}
				}
				entry.config.worldspaces.insert(entry.config.worldspaces.end(), file.worldspaces.begin(), file.worldspaces.end());
			}
			resolved.seasons.push_back(std::move(entry));
		}
		return resolved;
	}

	WorldSwaps SwapsIn(const ResolvedSeason& a_season, const StaticWorldSnapshot& a_snapshot, std::string_view a_worldspace)
	{
		WorldSwaps swaps;
		if (!a_season.config.Covers(a_worldspace)) return swaps;
		for (const auto& [from, to] : a_season.maps[SwapRecord::LandTextures]) {
			// GetSwapLandTexture: a target that is no land texture leaves the original.
			if (from != to && a_snapshot.landTexturesByFormID.contains(from) && a_snapshot.landTexturesByFormID.contains(to)) swaps.landTextures.emplace(from, to);
		}
		const auto& bases = a_snapshot.baseObjectsByFormID;
		for (const auto record : kBaseObjectSwapRecords) {
			if (!a_season.config.swaps[Index(record)]) continue;
			for (const auto& [from, to] : a_season.maps[record]) {
				// The map is chosen by the original base's type; the replacement must be a base object.
				const auto base = bases.find(from);
				if (base != bases.end() && base->second.signature == SwapRecordSignature(record) && from != to && bases.contains(to)) swaps.baseObjects.emplace(from, to);
			}
		}
		return swaps;
	}
}
