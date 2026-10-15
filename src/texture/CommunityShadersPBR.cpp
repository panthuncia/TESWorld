#include "tesworld/CommunityShadersPBR.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace tesworld::pbr
{
	namespace
	{
		bool IsTexturePath(std::string_view a_path)
		{
			return a_path.ends_with(".dds") || a_path.ends_with(".png") || a_path.ends_with(".tga");
		}

		float GetFloat(const nlohmann::json& a_json, std::string_view a_key, float a_fallback)
		{
			const auto it = a_json.find(std::string(a_key));
			return it != a_json.end() && it->is_number() ? it->get<float>() : a_fallback;
		}

		bool GetBool(const nlohmann::json& a_json, std::string_view a_key, bool a_fallback)
		{
			const auto it = a_json.find(std::string(a_key));
			return it != a_json.end() && it->is_boolean() ? it->get<bool>() : a_fallback;
		}

		std::array<float, 3> GetFloat3(const nlohmann::json& a_json, std::string_view a_key, std::array<float, 3> a_fallback)
		{
			const auto it = a_json.find(std::string(a_key));
			if (it == a_json.end() || !it->is_array() || it->size() < 3) return a_fallback;
			for (std::size_t i = 0; i < 3; ++i) {
				if (!(*it)[i].is_number()) return a_fallback;
				a_fallback[i] = (*it)[i].get<float>();
			}
			return a_fallback;
		}

		GlintParameters ParseGlint(const nlohmann::json& a_json)
		{
			GlintParameters glint;
			const auto it = a_json.find("glintParameters");
			if (it == a_json.end() || !it->is_object()) return glint;
			glint.enabled = GetBool(*it, "enabled", glint.enabled);
			glint.screenSpaceScale = GetFloat(*it, "screenSpaceScale", glint.screenSpaceScale);
			glint.logMicrofacetDensity = GetFloat(*it, "logMicrofacetDensity", glint.logMicrofacetDensity);
			glint.microfacetRoughness = GetFloat(*it, "microfacetRoughness", glint.microfacetRoughness);
			glint.densityRandomization = GetFloat(*it, "densityRandomization", glint.densityRandomization);
			return glint;
		}

		std::optional<nlohmann::json> ReadJson(const vfs::FileSystem& a_files, const std::string& a_path)
		{
			const auto bytes = a_files.Read(a_path);
			if (!bytes) return std::nullopt;
			auto json = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
			if (json.is_discarded()) return std::nullopt;
			return json;
		}

		// The file's name as the editor ID it describes (case kept from nothing: the paths are normalized, so lower).
		std::string StemOf(const std::string& a_path)
		{
			return std::string(vfs::FileStem(a_path));
		}
	}

	std::string DerivedTexturePath(std::string_view a_sourceTexturePath, DerivedTextureKind a_kind)
	{
		auto normalized = vfs::NormalizePath(a_sourceTexturePath);
		if (normalized.empty() || !IsTexturePath(normalized)) return {};
		if (normalized.starts_with("textures\\")) normalized.erase(0, 9);
		if (normalized.starts_with("pbr\\")) return {};
		const auto slash = normalized.find_last_of('\\');
		const auto dot = normalized.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
		auto stem = normalized.substr(0, dot);
		const auto extension = normalized.substr(dot);
		for (const std::string_view suffix : { "_d", "_diffuse", "_color" }) {
			if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
				stem.resize(stem.size() - suffix.size());
				break;
			}
		}
		const char* suffix = "";
		switch (a_kind) {
		case DerivedTextureKind::Normal: suffix = "_n"; break;
		case DerivedTextureKind::Displacement: suffix = "_p"; break;
		case DerivedTextureKind::RMAOS: suffix = "_rmaos"; break;
		default: break;
		}
		return "textures\\pbr\\" + stem + suffix + extension;
	}

	std::string TextureStemKey(std::string_view a_texturePath)
	{
		auto normalized = vfs::NormalizePath(a_texturePath);
		if (normalized.starts_with("textures\\")) normalized.erase(0, 9);
		if (normalized.starts_with("pbr\\")) normalized.erase(0, 4);
		const auto slash = normalized.find_last_of('\\');
		const auto dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) normalized.resize(dot);
		for (const std::string_view suffix : { "_rmaos", "_diffuse", "_color", "_n", "_p", "_d" }) {
			if (normalized.size() > suffix.size() && normalized.ends_with(suffix)) {
				normalized.resize(normalized.size() - suffix.size());
				break;
			}
		}
		return normalized;
	}

	Database::Database(const vfs::FileSystem& a_files)
	{
		for (const auto& path : a_files.Enumerate("pbrtexturesets", ".json")) {
			const auto json = ReadJson(a_files, path);
			if (!json || !json->is_object()) continue;
			TextureSetData data;
			data.editorID = StemOf(path);
			data.roughnessScale = GetFloat(*json, "roughnessScale", data.roughnessScale);
			data.displacementScale = GetFloat(*json, "displacementScale", data.displacementScale);
			data.specularLevel = GetFloat(*json, "specularLevel", data.specularLevel);
			data.subsurfaceColor = GetFloat3(*json, "subsurfaceColor", data.subsurfaceColor);
			data.subsurfaceOpacity = GetFloat(*json, "subsurfaceOpacity", data.subsurfaceOpacity);
			data.coatColor = GetFloat3(*json, "coatColor", data.coatColor);
			data.coatStrength = GetFloat(*json, "coatStrength", data.coatStrength);
			data.coatRoughness = GetFloat(*json, "coatRoughness", data.coatRoughness);
			data.coatSpecularLevel = GetFloat(*json, "coatSpecularLevel", data.coatSpecularLevel);
			data.innerLayerDisplacementOffset = GetFloat(*json, "innerLayerDisplacementOffset", data.innerLayerDisplacementOffset);
			data.fuzzColor = GetFloat3(*json, "fuzzColor", data.fuzzColor);
			data.fuzzWeight = GetFloat(*json, "fuzzWeight", data.fuzzWeight);
			data.glintParameters = ParseGlint(*json);
			_textureSets[data.editorID] = std::move(data);
		}
		for (const auto& path : a_files.Enumerate("pbrmaterialobjects", ".json")) {
			const auto json = ReadJson(a_files, path);
			if (!json || !json->is_object()) continue;
			MaterialObjectData data;
			data.editorID = StemOf(path);
			data.baseColorScale = GetFloat3(*json, "baseColorScale", data.baseColorScale);
			data.roughness = GetFloat(*json, "roughness", data.roughness);
			data.specularLevel = GetFloat(*json, "specularLevel", data.specularLevel);
			data.glintParameters = ParseGlint(*json);
			_materialObjects[data.editorID] = std::move(data);
		}
		for (const auto& path : a_files.Enumerate("pbrnifpatcher", ".json")) {
			const auto json = ReadJson(a_files, path);
			if (!json) continue;
			const nlohmann::json* records = nullptr;
			if (json->is_array()) {
				records = &*json;
			} else if (json->is_object()) {
				if (const auto it = json->find("entries"); it != json->end() && it->is_array()) records = &*it;
			}
			if (!records) continue;
			for (const auto& record : *records) {
				if (!record.is_object()) continue;
				const auto texture = record.find("texture");
				if (texture == record.end() || !texture->is_string()) continue;
				NifTextureData data;
				data.textureKey = TextureStemKey(texture->get<std::string>());
				if (data.textureKey.empty()) continue;
				if (const auto rename = record.find("rename"); rename != record.end() && rename->is_string()) {
					data.replacementTextureKey = TextureStemKey(rename->get<std::string>());
				}
				data.emissive = GetBool(record, "emissive", data.emissive);
				data.parallax = GetBool(record, "parallax", data.parallax);
				data.subsurface = GetBool(record, "subsurface", data.subsurface);
				data.subsurfaceFoliage = GetBool(record, "subsurface_foliage", data.subsurfaceFoliage);
				data.roughnessScale = GetFloat(record, "roughness_scale", data.roughnessScale);
				data.displacementScale = GetFloat(record, "displacement_scale", data.displacementScale);
				data.specularLevel = GetFloat(record, "specular_level", data.specularLevel);
				data.subsurfaceColor = GetFloat3(record, "subsurface_color", data.subsurfaceColor);
				data.subsurfaceOpacity = GetFloat(record, "subsurface_opacity", data.subsurfaceOpacity);
				if (const auto glint = record.find("glint"); glint != record.end() && glint->is_object()) {
					data.glintParameters.enabled = true;
					data.glintParameters.screenSpaceScale = GetFloat(*glint, "screen_space_scale", data.glintParameters.screenSpaceScale);
					data.glintParameters.logMicrofacetDensity = GetFloat(*glint, "log_microfacet_density", data.glintParameters.logMicrofacetDensity);
					data.glintParameters.microfacetRoughness = GetFloat(*glint, "microfacet_roughness", data.glintParameters.microfacetRoughness);
					data.glintParameters.densityRandomization = GetFloat(*glint, "density_randomization", data.glintParameters.densityRandomization);
				}
				const auto key = data.textureKey;
				_nifTextures[key] = std::move(data);
			}
		}
	}

	namespace
	{
		std::string Lower(std::string_view a_value)
		{
			std::string result(a_value);
			std::ranges::transform(result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return result;
		}
	}

	// Editor IDs are matched without case: the file names come back lower-cased from the file system.
	const TextureSetData* Database::FindTextureSet(std::string_view a_editorID) const
	{
		const auto it = _textureSets.find(Lower(a_editorID));
		return it == _textureSets.end() ? nullptr : &it->second;
	}

	const MaterialObjectData* Database::FindMaterialObject(std::string_view a_editorID) const
	{
		const auto it = _materialObjects.find(Lower(a_editorID));
		return it == _materialObjects.end() ? nullptr : &it->second;
	}

	const NifTextureData* Database::FindNifTexture(std::string_view a_texturePath) const
	{
		const auto key = TextureStemKey(a_texturePath);
		const auto it = key.empty() ? _nifTextures.end() : _nifTextures.find(key);
		return it == _nifTextures.end() ? nullptr : &it->second;
	}

	std::string PreferDerivedTexturePath(const vfs::FileSystem& a_files, std::string_view a_sourceTexturePath, DerivedTextureKind a_kind)
	{
		const auto derived = DerivedTexturePath(a_sourceTexturePath, a_kind);
		if (!derived.empty() && a_files.Exists(derived)) {
			return derived;
		}
		return vfs::NormalizePath(a_sourceTexturePath);
	}
}
