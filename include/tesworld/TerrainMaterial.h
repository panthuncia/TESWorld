#pragma once

#include "tesworld/CommunityShadersPBR.h"
#include "tesworld/Plugins.h"
#include "tesworld/Stochastic.h"
#include "tesworld/Texture.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The painted landscape's material, evaluated on the CPU as BasicRenderer's terrain shader evaluates it
// (ApplyTerrainMaterialInternal, terrainCommon.hlsli) when it bakes its runtime virtual texture: each region's layer
// weights from LAND's 17x17 quadrant lattice through a cubic B-spline, each layer's textures sampled at the footprint
// (directly, hex-tiled after Mikkelsen, or through SARP's Gaussian stochastic textures), colours blended by weight and
// normals as partial derivatives. A bake asks for a world position and a texel size.
namespace tesworld::texture
{
	inline constexpr std::uint32_t kTerrainLayerSnow = 1u << 0;
	inline constexpr std::uint32_t kTerrainLayerHeightFromDiffuseAlpha = 1u << 1;
	inline constexpr std::uint32_t kTerrainLayerPbr = 1u << 2;
	inline constexpr std::uint32_t kTerrainLayerGlint = 1u << 3;
	inline constexpr float kTerrainRegionSize = 2048.0f;
	inline constexpr float kTerrainDefaultStochasticScale = 3.4641016f;

	struct TerrainLayerTextures
	{
		std::shared_ptr<const MipChain> diffuse;
		// Direct3D's convention (green down), as SARP converts Skyrim's.
		std::shared_ptr<const MipChain> normal;
		std::shared_ptr<const MipChain> rmaos;
		std::shared_ptr<const StochasticTexture> diffuseStochastic;
		std::shared_ptr<const StochasticTexture> normalStochastic;
		std::uint32_t flags{ 0 };
		float uvScale{ kSkyrimDefaultTerrainLayerUvScale };
		float roughnessScale{ 1.0f };
		float stochasticScale{ kTerrainDefaultStochasticScale };
		Vec4 fallbackColor{ 0.45f, 0.42f, 0.36f, 1.0f };
	};

	// BasicRenderer's terrain switches, at its defaults.
	struct TerrainMaterialSettings
	{
		bool stochastic{ true };
		bool stochasticDiffuse{ true };
		bool stochasticNormal{ true };
		bool derivativeNormals{ true };
		bool gaussian{ true };
		float blendCurve{ 0.65f };
	};

	struct TerrainMaterialSample
	{
		Vec3 albedo{ 0.0f, 0.0f, 0.0f };
		// In the terrain's tangent space: x east, y north, z away from the ground. Turn it with TerrainBasis.
		Vec3 normal{ 0.0f, 0.0f, 1.0f };
		float roughness{ 0.9f };
		float metallic{ 0.0f };
		float ambientOcclusion{ 1.0f };
		bool valid{ false };
	};

	// Keeps references to the build result's regions and weights; it must outlive the evaluator.
	class TerrainMaterial
	{
	public:
		TerrainMaterial(const TerrainWorldBuildResult& a_terrain, std::vector<TerrainLayerTextures> a_layers, TerrainMaterialSettings a_settings = {});

		// The material at a Skyrim position, filtered over a square texel a_texelWorldSize units wide.
		[[nodiscard]] TerrainMaterialSample Evaluate(Vec2 a_position, float a_texelWorldSize) const;
		[[nodiscard]] const std::vector<TerrainLayerTextures>& Layers() const { return _layers; }
		[[nodiscard]] const TerrainWorldBuildResult& Terrain() const { return _terrain; }
		[[nodiscard]] const TerrainMaterialSettings& Settings() const { return _settings; }

	private:
		[[nodiscard]] float LayerWeight(const TerrainRegionDescriptor& a_region, std::uint32_t a_localLayer, Vec2 a_regionLocal) const;

		const TerrainWorldBuildResult& _terrain;
		std::vector<TerrainLayerTextures> _layers;
		TerrainMaterialSettings _settings;
		std::unordered_map<std::uint64_t, std::size_t> _regionIndex;
	};

	// The tangent frame BasicRenderer's terrain shader puts on a ground normal (TerrainBasis), in Skyrim's Z-up space:
	// tangent = north x normal, bitangent = normal x tangent. Returns the tangent-space normal turned onto it.
	[[nodiscard]] Vec3 FromTerrainTangentSpace(Vec3 a_tangentNormal, Vec3 a_groundNormal);

	// Decoded layer textures, shared between LoadTerrainLayers calls with the same options: a season's material would
	// otherwise read the default's textures again. Safe from any thread.
	class TerrainTextureCache
	{
	public:
		struct Entry
		{
			std::shared_ptr<const MipChain> chain;
			std::shared_ptr<const StochasticTexture> stochastic;
			std::string error;
		};

		[[nodiscard]] std::optional<Entry> Find(const std::string& a_path, bool a_normal) const
		{
			std::scoped_lock lock(_mutex);
			const auto it = _entries.find({ a_path, a_normal });
			return it == _entries.end() ? std::nullopt : std::optional<Entry>(it->second);
		}

		void Store(const std::string& a_path, bool a_normal, Entry a_entry)
		{
			std::scoped_lock lock(_mutex);
			_entries.insert_or_assign({ a_path, a_normal }, std::move(a_entry));
		}

	private:
		mutable std::mutex _mutex;
		std::map<std::pair<std::string, bool>, Entry> _entries;
	};

	struct TerrainLayerLoadOptions
	{
		// Textures are read only down from this size: a bake never samples finer than its texel.
		std::uint32_t maxTextureSize{ 512 };
		bool stochastic{ true };
		bool gaussian{ true };
		std::uint32_t lutWidth{ 256 };
		bool loadRmaos{ false };
		// Shared with other calls made with the same options, or none.
		TerrainTextureCache* cache{ nullptr };
	};

	// Each layer's textures, found as SARP's TerrainRuntime finds them: the TXST's diffuse and normal, Community Shaders'
	// textures\pbr\ versions for its PBR texture sets, terrain-LOD-looking paths ignored, missing files left to the
	// layer's fallback colour (a warning each).
	[[nodiscard]] std::vector<TerrainLayerTextures> LoadTerrainLayers(
		const TerrainWorldBuildResult& a_terrain,
		const vfs::FileSystem& a_files,
		const pbr::Database* a_pbr,
		const TerrainLayerLoadOptions& a_options,
		std::vector<std::string>* a_warnings = nullptr);
}
