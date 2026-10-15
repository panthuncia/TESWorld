#pragma once

#include "tesworld/Terrain.h"
#include "tesworld/TerrainMaterial.h"
#include "tesworld/Texture.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Terrain LOD's textures: a square of a worldspace's painted landscape baked to a diffuse and a model-space normal map
// and encoded to DDS. The material (TerrainMaterial) gives each texel's colour and tangent-space normal; the bake turns
// the normal onto the landscape's own (from the heights), tints the colour with LAND's vertex colours and brightens it
// as the Creation Kit's LOD textures are. A baker runs on the CPU, or on the GPU (TerrainBakeGpu.h) when there is one.
namespace tesworld::texture
{
	struct TerrainBakeSettings
	{
		bool vertexColors{ true };
		// colour * gain + bias, after the tint.
		float gain{ 1.0f };
		float bias{ 0.0f };
		// Where no layer paints.
		Vec3 fallbackAlbedo{ 0.45f, 0.42f, 0.36f };
	};

	struct TerrainBakeRequest
	{
		// The square's south-west corner and side, in Skyrim units. Row 0 of each image is its north edge.
		Vec2 origin{ 0.0f, 0.0f };
		float extent{ 0.0f };
		std::uint32_t diffuseSize{ 0 };
		std::uint32_t normalSize{ 0 };
		// A material the baker was given (AddMaterial).
		std::uint32_t material{ 0 };
		Format diffuseFormat{ Format::BC7 };
		Format normalFormat{ Format::BC7 };
	};

	struct TerrainBakeResult
	{
		std::vector<std::uint8_t> diffuseDds;
		std::vector<std::uint8_t> normalDds;
		std::string error;

		[[nodiscard]] bool Ok() const { return error.empty(); }
	};

	class TerrainBaker
	{
	public:
		using Callback = std::function<void(TerrainBakeResult)>;

		virtual ~TerrainBaker() = default;

		// The material is read until the baker is gone; it must outlive it.
		virtual std::uint32_t AddMaterial(const TerrainMaterial& a_material) = 0;
		// Bakes, then calls a_done, on this thread or another: a baker may batch requests and finish them in any order.
		virtual void Bake(const TerrainBakeRequest& a_request, Callback a_done) = 0;
		// Returns once every request so far has called back.
		virtual void Wait() = 0;
		// "CPU", or the GPU's API and adapter.
		[[nodiscard]] virtual std::string Backend() const = 0;
		// What went wrong that the baker worked around (a GPU that failed part way), or nothing.
		[[nodiscard]] virtual std::string Problem() const { return {}; }
	};

	// Bakes on the calling thread (its rows in parallel) and calls back before returning.
	[[nodiscard]] std::unique_ptr<TerrainBaker> CreateCpuTerrainBaker(const WorldTerrain& a_terrain, TerrainBakeSettings a_settings);

	// The images before encoding, as the CPU baker makes them; either may be null. When both are the same size the
	// material is evaluated once a texel.
	void BakeTerrainImages(const TerrainMaterial& a_material, const WorldTerrain& a_terrain, const TerrainBakeSettings& a_settings, Vec2 a_origin, float a_extent,
		Image* a_diffuse, Image* a_normal);

	// Skyrim's terrain LOD normal maps are model space with green up: (x, z, y) * 0.5 + 0.5.
	[[nodiscard]] Vec4 EncodeSkyrimTerrainNormal(Vec3 a_normal);
	[[nodiscard]] Vec3 DecodeSkyrimTerrainNormal(Vec4 a_encoded);
}
