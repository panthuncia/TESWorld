#pragma once

#include "tesworld/Vfs.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

// Community Shaders' PBR data (moved from SARP): the JSON in PBRTextureSets, PBRMaterialObjects and PBRNifPatcher, and
// the textures\pbr\ paths its textures are derived to.
namespace tesworld::pbr
{
	struct GlintParameters
	{
		bool enabled{ false };
		float screenSpaceScale{ 1.5f };
		float logMicrofacetDensity{ 40.0f };
		float microfacetRoughness{ 0.015f };
		float densityRandomization{ 2.0f };

		[[nodiscard]] std::array<float, 4> PackedForShader() const
		{
			return { screenSpaceScale, 40.0f - logMicrofacetDensity, microfacetRoughness, densityRandomization };
		}
	};

	struct TextureSetData
	{
		std::string editorID;
		float roughnessScale{ 1.0f };
		float displacementScale{ 1.0f };
		float specularLevel{ 0.04f };
		std::array<float, 3> subsurfaceColor{ 1.0f, 1.0f, 1.0f };
		float subsurfaceOpacity{ 0.0f };
		std::array<float, 3> coatColor{ 1.0f, 1.0f, 1.0f };
		float coatStrength{ 1.0f };
		float coatRoughness{ 1.0f };
		float coatSpecularLevel{ 0.04f };
		float innerLayerDisplacementOffset{ 0.0f };
		std::array<float, 3> fuzzColor{ 1.0f, 1.0f, 1.0f };
		float fuzzWeight{ 0.0f };
		GlintParameters glintParameters;
	};

	struct MaterialObjectData
	{
		std::string editorID;
		std::array<float, 3> baseColorScale{ 1.0f, 1.0f, 1.0f };
		float roughness{ 1.0f };
		float specularLevel{ 1.0f };
		GlintParameters glintParameters;
	};

	struct NifTextureData
	{
		std::string textureKey;
		std::string replacementTextureKey;
		bool emissive{ false };
		bool parallax{ false };
		bool subsurface{ false };
		bool subsurfaceFoliage{ false };
		float roughnessScale{ 1.0f };
		float displacementScale{ 1.0f };
		float specularLevel{ 0.04f };
		std::array<float, 3> subsurfaceColor{ 1.0f, 1.0f, 1.0f };
		float subsurfaceOpacity{ 0.0f };
		GlintParameters glintParameters;
	};

	enum class DerivedTextureKind
	{
		BaseColor,
		Normal,
		Displacement,
		RMAOS
	};

	// textures\pbr\<stem>[_n|_p|_rmaos].dds for a source texture (its _d/_diffuse/_color suffix dropped); empty for a
	// path already under pbr\ or not a texture.
	[[nodiscard]] std::string DerivedTexturePath(std::string_view a_sourceTexturePath, DerivedTextureKind a_kind);
	// The texture's stem without textures\, pbr\ or a known suffix: the key the NIF patcher's records use.
	[[nodiscard]] std::string TextureStemKey(std::string_view a_texturePath);

	class Database
	{
	public:
		Database() = default;
		// Reads every JSON under the three folders the game sees (malformed files are skipped).
		explicit Database(const vfs::FileSystem& a_files);

		[[nodiscard]] const TextureSetData* FindTextureSet(std::string_view a_editorID) const;
		[[nodiscard]] const MaterialObjectData* FindMaterialObject(std::string_view a_editorID) const;
		[[nodiscard]] const NifTextureData* FindNifTexture(std::string_view a_texturePath) const;
		[[nodiscard]] bool Empty() const { return _textureSets.empty() && _materialObjects.empty() && _nifTextures.empty(); }

	private:
		std::unordered_map<std::string, TextureSetData> _textureSets;
		std::unordered_map<std::string, MaterialObjectData> _materialObjects;
		std::unordered_map<std::string, NifTextureData> _nifTextures;
	};

	// The derived path when the game has that file, else the source path normalized.
	[[nodiscard]] std::string PreferDerivedTexturePath(const vfs::FileSystem& a_files, std::string_view a_sourceTexturePath, DerivedTextureKind a_kind);
}
