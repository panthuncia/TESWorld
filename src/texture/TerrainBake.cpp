#include "tesworld/TerrainBake.h"

#include "tesworld/Profiling.h"

#include <cmath>
#include <format>

#include <oneapi/tbb/parallel_for.h>

namespace tesworld::texture
{
	namespace
	{
		// Bilinear across the lattice: per-vertex values the textures blend between.
		template <class F>
		auto LatticeBilinear(float a_x, float a_y, F&& a_at)
		{
			const float fx = a_x / kLandVertexSpacing;
			const float fy = a_y / kLandVertexSpacing;
			const auto gx = static_cast<std::int32_t>(std::floor(fx));
			const auto gy = static_cast<std::int32_t>(std::floor(fy));
			const float tx = fx - static_cast<float>(gx);
			const float ty = fy - static_cast<float>(gy);
			const auto a = a_at(gx, gy), b = a_at(gx + 1, gy), c = a_at(gx, gy + 1), d = a_at(gx + 1, gy + 1);
			return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
		}

		Vec4 DiffuseTexel(const TerrainMaterialSample& a_sample, const WorldTerrain& a_terrain, const TerrainBakeSettings& a_settings, Vec2 a_position)
		{
			Vec3 colour = a_sample.valid ? a_sample.albedo : a_settings.fallbackAlbedo;
			if (a_settings.vertexColors) {
				const auto tint = LatticeBilinear(a_position.x, a_position.y, [&](std::int32_t gx, std::int32_t gy) { return a_terrain.ColorAt(gx, gy); });
				colour = { colour.x * tint.x, colour.y * tint.y, colour.z * tint.z };
			}
			const float gain = a_settings.gain, bias = a_settings.bias;
			colour = { colour.x * gain + bias, colour.y * gain + bias, colour.z * gain + bias };
			return { colour.x, colour.y, colour.z, 1.0f };
		}

		Vec4 NormalTexel(const TerrainMaterialSample& a_sample, const WorldTerrain& a_terrain, Vec2 a_position)
		{
			const Vec3 ground = Normalize(LatticeBilinear(a_position.x, a_position.y, [&](std::int32_t gx, std::int32_t gy) { return a_terrain.NormalAt(gx, gy); }));
			const auto normal = a_sample.valid ? FromTerrainTangentSpace(a_sample.normal, ground) : ground;
			return EncodeSkyrimTerrainNormal(normal);
		}

		class CpuTerrainBaker final : public TerrainBaker
		{
		public:
			CpuTerrainBaker(const WorldTerrain& a_terrain, TerrainBakeSettings a_settings) :
				_terrain(a_terrain),
				_settings(a_settings)
			{}

			std::uint32_t AddMaterial(const TerrainMaterial& a_material) override
			{
				_materials.push_back(&a_material);
				return static_cast<std::uint32_t>(_materials.size() - 1);
			}

			void Bake(const TerrainBakeRequest& a_request, Callback a_done) override
			{
				TW_ZONE("CPU terrain bake");
				TerrainBakeResult result;
				if (a_request.material >= _materials.size()) {
					result.error = "no such material";
					a_done(std::move(result));
					return;
				}
				Image diffuse(a_request.diffuseSize, a_request.diffuseSize);
				Image normal(a_request.normalSize, a_request.normalSize);
				BakeTerrainImages(*_materials[a_request.material], _terrain, _settings, a_request.origin, a_request.extent, &diffuse, &normal);
				std::string error;
				auto encodedDiffuse = EncodeDds(BuildMips(std::move(diffuse)), a_request.diffuseFormat, &error);
				auto encodedNormal = encodedDiffuse ? EncodeDds(BuildMips(std::move(normal)), a_request.normalFormat, &error) : std::nullopt;
				if (!encodedDiffuse || !encodedNormal) {
					result.error = error.empty() ? std::string("encoding failed") : error;
				} else {
					result.diffuseDds = std::move(*encodedDiffuse);
					result.normalDds = std::move(*encodedNormal);
				}
				a_done(std::move(result));
			}

			void Wait() override {}

			[[nodiscard]] std::string Backend() const override { return "CPU"; }

		private:
			const WorldTerrain& _terrain;
			TerrainBakeSettings _settings;
			std::vector<const TerrainMaterial*> _materials;
		};
	}

	std::unique_ptr<TerrainBaker> CreateCpuTerrainBaker(const WorldTerrain& a_terrain, TerrainBakeSettings a_settings)
	{
		return std::make_unique<CpuTerrainBaker>(a_terrain, a_settings);
	}

	void BakeTerrainImages(const TerrainMaterial& a_material, const WorldTerrain& a_terrain, const TerrainBakeSettings& a_settings, Vec2 a_origin, float a_extent,
		Image* a_diffuse, Image* a_normal)
	{
		const auto positionOf = [&](std::uint32_t a_px, std::uint32_t a_py, float a_texel) {
			// Row 0 is the square's north edge: the mesh's v runs 1 - y.
			return Vec2{ a_origin.x + (a_px + 0.5f) * a_texel, a_origin.y + a_extent - (a_py + 0.5f) * a_texel };
		};
		// One image, or both at once when they're the same size: the material is the same at the same texel.
		const auto bake = [&](Image* a_colour, Image* a_normals) {
			const auto& image = a_colour ? *a_colour : *a_normals;
			TW_ZONE_TEXT("BakeTerrainImages", "{}{}{} {}x{}", a_colour ? "diffuse" : "", a_colour && a_normals ? "+" : "", a_normals ? "normal" : "", image.width, image.height);
			const float texel = a_extent / static_cast<float>(image.width);
			oneapi::tbb::parallel_for(std::uint32_t{ 0 }, image.height, [&](std::uint32_t py) {
				TW_ZONE("bake row");
				for (std::uint32_t px = 0; px < image.width; ++px) {
					const auto position = positionOf(px, py, texel);
					const auto sample = a_material.Evaluate(position, texel);
					if (a_colour) a_colour->SetTexel(px, py, DiffuseTexel(sample, a_terrain, a_settings, position));
					if (a_normals) a_normals->SetTexel(px, py, NormalTexel(sample, a_terrain, position));
				}
			});
		};
		const bool both = a_diffuse && a_normal && !a_diffuse->Empty() && !a_normal->Empty();
		if (both && a_diffuse->width == a_normal->width && a_diffuse->height == a_normal->height) {
			bake(a_diffuse, a_normal);
			return;
		}
		if (a_diffuse && !a_diffuse->Empty()) bake(a_diffuse, nullptr);
		if (a_normal && !a_normal->Empty()) bake(nullptr, a_normal);
	}

	Vec4 EncodeSkyrimTerrainNormal(Vec3 a_normal)
	{
		const auto n = Normalize(a_normal);
		return { n.x * 0.5f + 0.5f, n.z * 0.5f + 0.5f, n.y * 0.5f + 0.5f, 1.0f };
	}

	Vec3 DecodeSkyrimTerrainNormal(Vec4 a_encoded)
	{
		return Normalize({ a_encoded.x * 2.0f - 1.0f, a_encoded.z * 2.0f - 1.0f, a_encoded.y * 2.0f - 1.0f });
	}
}
