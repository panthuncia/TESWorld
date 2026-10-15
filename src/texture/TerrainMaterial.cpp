#include "tesworld/TerrainMaterial.h"

#include "tesworld/Profiling.h"

#include <algorithm>
#include <bit>
#include <cmath>

// A line-by-line port of BasicRenderer's terrainCommon.hlsli (the RVT-generation path: no parallax, no RVT lookup).
// Function names follow the shader's so the two can be compared.
namespace tesworld::texture
{
	namespace
	{
		struct StochasticContext
		{
			Vec3 weights{ 1.0f, 0.0f, 0.0f };
			Vec2 offsets0{};
			Vec2 offsets1{};
			Vec2 offsets2{};
			Vec2 uv{};
			Vec2 duDx{};
			Vec2 duDy{};
			float lod{ 0.0f };
		};

		float Saturate(float a_value) { return std::clamp(a_value, 0.0f, 1.0f); }
		Vec2 Add(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
		Vec2 Scale(Vec2 a, float s) { return { a.x * s, a.y * s }; }

		std::uint32_t TerrainHash(std::uint32_t a_x, std::uint32_t a_y)
		{
			std::uint32_t x = a_x * 1664525u + 1013904223u;
			std::uint32_t y = a_y * 1664525u + 1013904223u;
			x += y * 1664525u;
			y += x * 1013904223u;
			x ^= x >> 16u;
			y ^= y >> 16u;
			x *= 2246822519u;
			y *= 2246822519u;
			x ^= x >> 13u;
			y ^= y >> 13u;
			return x ^ y;
		}

		Vec2 TerrainHash2(std::int32_t a_x, std::int32_t a_y)
		{
			const auto h0 = TerrainHash(std::bit_cast<std::uint32_t>(a_x), std::bit_cast<std::uint32_t>(a_y));
			const auto h1 = TerrainHash(std::bit_cast<std::uint32_t>(a_x + 37), std::bit_cast<std::uint32_t>(a_y + 119));
			return { static_cast<float>(h0 & 0x00ffffffu) / 16777216.0f, static_cast<float>(h1 & 0x00ffffffu) / 16777216.0f };
		}

		float TerrainBias(float a_value, float a_bias)
		{
			a_value = Saturate(a_value);
			a_bias = std::clamp(a_bias, 0.001f, 0.999f);
			return a_value / (((1.0f / a_bias) - 2.0f) * (1.0f - a_value) + 1.0f);
		}

		float TerrainGain(float a_value, float a_gain)
		{
			a_value = Saturate(a_value);
			a_gain = Saturate(a_gain);
			if (std::abs(a_gain - 0.5f) < 1.0e-4f) return a_value;
			return a_value < 0.5f ? 0.5f * TerrainBias(a_value * 2.0f, 1.0f - a_gain) : 1.0f - 0.5f * TerrainBias(2.0f - a_value * 2.0f, 1.0f - a_gain);
		}

		Vec3 TerrainNormalizeWeights(Vec3 a_weights)
		{
			a_weights = { (std::max)(a_weights.x, 0.0f), (std::max)(a_weights.y, 0.0f), (std::max)(a_weights.z, 0.0f) };
			const float sum = a_weights.x + a_weights.y + a_weights.z;
			return sum > 1.0e-5f ? a_weights * (1.0f / sum) : Vec3{ 1.0f, 0.0f, 0.0f };
		}

		Vec3 TerrainShapeStochasticWeights(Vec3 a_weights, float a_blendCurve)
		{
			a_blendCurve = Saturate(a_blendCurve);
			const float sharp = Saturate((a_blendCurve - 0.5f) * 2.0f);
			const float soft = Saturate((0.5f - a_blendCurve) * 2.0f);
			float gamma = 1.0f + (7.0f - 1.0f) * sharp;
			gamma = gamma + (0.5f - gamma) * soft;
			auto shaped = TerrainNormalizeWeights({
				std::pow((std::max)(a_weights.x, 1.0e-5f), gamma),
				std::pow((std::max)(a_weights.y, 1.0e-5f), gamma),
				std::pow((std::max)(a_weights.z, 1.0e-5f), gamma),
			});
			shaped = { TerrainGain(shaped.x, a_blendCurve), TerrainGain(shaped.y, a_blendCurve), TerrainGain(shaped.z, a_blendCurve) };
			return TerrainNormalizeWeights(shaped);
		}

		StochasticContext TerrainBuildStochasticContext(Vec2 a_uv, Vec2 a_duDx, Vec2 a_duDy, float a_scale, float a_blendCurve, float a_lod)
		{
			StochasticContext ctx;
			ctx.uv = a_uv;
			ctx.duDx = a_duDx;
			ctx.duDy = a_duDy;
			ctx.lod = (std::max)(a_lod, 0.0f);
			const float scale = (std::max)(a_scale, 0.001f);
			const Vec2 grid = Scale(a_uv, scale);
			const Vec2 skewed{ grid.x - 0.57735027f * grid.y, 1.15470054f * grid.y };
			const auto baseX = static_cast<std::int32_t>(std::floor(skewed.x));
			const auto baseY = static_cast<std::int32_t>(std::floor(skewed.y));
			const float fx = skewed.x - static_cast<float>(baseX);
			const float fy = skewed.y - static_cast<float>(baseY);
			const float z = 1.0f - fx - fy;
			Vec3 w;
			if (z > 0.0f) {
				ctx.offsets0 = TerrainHash2(baseX, baseY);
				ctx.offsets1 = TerrainHash2(baseX, baseY + 1);
				ctx.offsets2 = TerrainHash2(baseX + 1, baseY);
				w = { z, fy, fx };
			} else {
				ctx.offsets0 = TerrainHash2(baseX + 1, baseY + 1);
				ctx.offsets1 = TerrainHash2(baseX + 1, baseY);
				ctx.offsets2 = TerrainHash2(baseX, baseY + 1);
				w = { -z, 1.0f - fy, 1.0f - fx };
			}
			ctx.weights = TerrainShapeStochasticWeights(w, a_blendCurve);
			return ctx;
		}

		Vec4 Sample(const MipChain& a_chain, Vec2 a_uv, Vec2 a_duDx, Vec2 a_duDy)
		{
			return SampleGrad(a_chain, a_uv, a_duDx, a_duDy, Address::Wrap);
		}

		float VarianceScale(Vec3 a_weights)
		{
			return 1.0f / std::sqrt((std::max)(Dot(a_weights, a_weights), 1.0e-4f));
		}

		float Blend1(float a, float b, float c, Vec3 w)
		{
			return Saturate(((a - 0.5f) * w.x + (b - 0.5f) * w.y + (c - 0.5f) * w.z) * VarianceScale(w) + 0.5f);
		}

		float LutY(float a_lod, float a_lutHeight)
		{
			return Saturate(((std::max)(a_lod, 0.0f) + 0.5f) / (std::max)(a_lutHeight, 1.0f));
		}

		// The inverse LUT read with a clamping bilinear sampler at level 0, one channel per component.
		float InverseLut(const StochasticTexture& a_texture, float a_gaussian, float a_lod, int a_channel)
		{
			const auto value = SampleBilinear(a_texture.inverseLut, { a_gaussian, LutY(a_lod, static_cast<float>(a_texture.lutHeight)) }, Address::Clamp);
			return a_channel == 0 ? value.x : (a_channel == 1 ? value.y : value.z);
		}

		Vec3 TerrainSampleStochasticDiffuse(const StochasticTexture& a_texture, const StochasticContext& ctx)
		{
			const auto g0 = Sample(a_texture.gaussian, Add(ctx.uv, ctx.offsets0), ctx.duDx, ctx.duDy);
			const auto g1 = Sample(a_texture.gaussian, Add(ctx.uv, ctx.offsets1), ctx.duDx, ctx.duDy);
			const auto g2 = Sample(a_texture.gaussian, Add(ctx.uv, ctx.offsets2), ctx.duDx, ctx.duDy);
			Vec3 color{
				InverseLut(a_texture, Blend1(g0.x, g1.x, g2.x, ctx.weights), ctx.lod, 0),
				InverseLut(a_texture, Blend1(g0.y, g1.y, g2.y, ctx.weights), ctx.lod, 1),
				InverseLut(a_texture, Blend1(g0.z, g1.z, g2.z, ctx.weights), ctx.lod, 2),
			};
			if (a_texture.mode == StochasticMode::DecorrelatedColor) {
				color = a_texture.colorSpaceOrigin + a_texture.colorSpaceVector0 * color.x + a_texture.colorSpaceVector1 * color.y + a_texture.colorSpaceVector2 * color.z;
			}
			return { Saturate(color.x), Saturate(color.y), Saturate(color.z) };
		}

		Vec3 TerrainSampleMikkelsenDiffuse(const MipChain& a_texture, const StochasticContext& ctx)
		{
			const auto c0 = Sample(a_texture, Add(ctx.uv, ctx.offsets0), ctx.duDx, ctx.duDy);
			const auto c1 = Sample(a_texture, Add(ctx.uv, ctx.offsets1), ctx.duDx, ctx.duDy);
			const auto c2 = Sample(a_texture, Add(ctx.uv, ctx.offsets2), ctx.duDx, ctx.duDy);
			const auto& w = ctx.weights;
			return {
				Saturate(c0.x * w.x + c1.x * w.y + c2.x * w.z),
				Saturate(c0.y * w.x + c1.y * w.y + c2.y * w.z),
				Saturate(c0.z * w.x + c1.z * w.y + c2.z * w.z),
			};
		}

		Vec3 TerrainDecodeNormal(Vec4 a_encoded)
		{
			return Normalize({ a_encoded.x * 2.0f - 1.0f, a_encoded.y * 2.0f - 1.0f, a_encoded.z * 2.0f - 1.0f });
		}

		Vec3 TerrainSampleStochasticNormal(const StochasticTexture& a_texture, const StochasticContext& ctx)
		{
			const auto g0 = Sample(a_texture.gaussian, Add(ctx.uv, ctx.offsets0), ctx.duDx, ctx.duDy);
			const auto g1 = Sample(a_texture.gaussian, Add(ctx.uv, ctx.offsets1), ctx.duDx, ctx.duDy);
			const auto g2 = Sample(a_texture.gaussian, Add(ctx.uv, ctx.offsets2), ctx.duDx, ctx.duDy);
			const float x = InverseLut(a_texture, Blend1(g0.x, g1.x, g2.x, ctx.weights), ctx.lod, 0) * 2.0f - 1.0f;
			const float y = InverseLut(a_texture, Blend1(g0.y, g1.y, g2.y, ctx.weights), ctx.lod, 1) * 2.0f - 1.0f;
			return Normalize({ x, y, std::sqrt(Saturate(1.0f - x * x - y * y)) });
		}

		Vec2 TerrainNormalToDerivative(Vec3 a_normal)
		{
			a_normal = Normalize(a_normal);
			const float nz = (std::max)(a_normal.z, 0.05f);
			return { -a_normal.x / nz, -a_normal.y / nz };
		}

		Vec3 TerrainDerivativeToNormal(Vec2 a_derivative)
		{
			return Normalize({ -a_derivative.x, -a_derivative.y, 1.0f });
		}

		Vec3 TerrainSampleStochasticNormalDerivative(const MipChain& a_texture, const StochasticContext& ctx)
		{
			const auto d0 = TerrainNormalToDerivative(TerrainDecodeNormal(Sample(a_texture, Add(ctx.uv, ctx.offsets0), ctx.duDx, ctx.duDy)));
			const auto d1 = TerrainNormalToDerivative(TerrainDecodeNormal(Sample(a_texture, Add(ctx.uv, ctx.offsets1), ctx.duDx, ctx.duDy)));
			const auto d2 = TerrainNormalToDerivative(TerrainDecodeNormal(Sample(a_texture, Add(ctx.uv, ctx.offsets2), ctx.duDx, ctx.duDy)));
			const auto& w = ctx.weights;
			return TerrainDerivativeToNormal({ d0.x * w.x + d1.x * w.y + d2.x * w.z, d0.y * w.x + d1.y * w.y + d2.y * w.z });
		}

		float TerrainCubicBSpline(float p0, float p1, float p2, float p3, float t)
		{
			const float t2 = t * t;
			const float t3 = t2 * t;
			const float w0 = (1.0f - 3.0f * t + 3.0f * t2 - t3) * (1.0f / 6.0f);
			const float w1 = (4.0f - 6.0f * t2 + 3.0f * t3) * (1.0f / 6.0f);
			const float w2 = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) * (1.0f / 6.0f);
			const float w3 = t3 * (1.0f / 6.0f);
			return p0 * w0 + p1 * w1 + p2 * w2 + p3 * w3;
		}

		std::uint64_t RegionKey(std::int32_t a_x, std::int32_t a_y)
		{
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32) | static_cast<std::uint32_t>(a_y);
		}
	}

	TerrainMaterial::TerrainMaterial(const TerrainWorldBuildResult& a_terrain, std::vector<TerrainLayerTextures> a_layers, TerrainMaterialSettings a_settings) :
		_terrain(a_terrain),
		_layers(std::move(a_layers)),
		_settings(a_settings)
	{
		TW_ZONE("TerrainMaterial index");
		_regionIndex.reserve(a_terrain.regions.size());
		for (std::size_t i = 0; i < a_terrain.regions.size(); ++i) {
			_regionIndex.emplace(RegionKey(a_terrain.regions[i].regionX, a_terrain.regions[i].regionY), i);
		}
	}

	float TerrainMaterial::LayerWeight(const TerrainRegionDescriptor& a_region, std::uint32_t a_localLayer, Vec2 a_regionLocal) const
	{
		// TerrainInterpolateLayerWeight: the 17x17 lattice (with its one-sample gutter) through a cubic B-spline.
		const float gridX = Saturate(a_regionLocal.x / kTerrainRegionSize) * 16.0f;
		const float gridY = Saturate(a_regionLocal.y / kTerrainRegionSize) * 16.0f;
		const auto baseX = static_cast<std::int32_t>(std::floor(gridX));
		const auto baseY = static_cast<std::int32_t>(std::floor(gridY));
		const float fx = gridX - static_cast<float>(baseX);
		const float fy = gridY - static_cast<float>(baseY);
		const auto side = static_cast<std::int32_t>(a_region.weightSampleSide);
		const auto maxSample = side - 2;
		const auto samplesPerLayer = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
		const auto wordsPerLayer = (samplesPerLayer + 3) / 4;
		const auto layerWordBase = static_cast<std::size_t>(a_region.weightBlockStart) + a_localLayer * wordsPerLayer;
		const auto load = [&](std::int32_t a_x, std::int32_t a_y) {
			const auto x = static_cast<std::size_t>(std::clamp(a_x, -1, maxSample) + 1);
			const auto y = static_cast<std::size_t>(std::clamp(a_y, -1, maxSample) + 1);
			const auto sample = y * static_cast<std::size_t>(side) + x;
			const auto word = layerWordBase + sample / 4;
			if (word >= _terrain.weightBlocks.size()) return 0.0f;
			return static_cast<float>((_terrain.weightBlocks[word] >> ((sample & 3u) * 8u)) & 0xFFu) / 255.0f;
		};
		float rows[4];
		for (int r = 0; r < 4; ++r) {
			const auto y = baseY - 1 + r;
			rows[r] = TerrainCubicBSpline(load(baseX - 1, y), load(baseX, y), load(baseX + 1, y), load(baseX + 2, y), fx);
		}
		return Saturate(TerrainCubicBSpline(rows[0], rows[1], rows[2], rows[3], fy));
	}

	TerrainMaterialSample TerrainMaterial::Evaluate(Vec2 a_position, float a_texelWorldSize) const
	{
		TerrainMaterialSample result;
		const auto regionX = static_cast<std::int32_t>(std::floor(a_position.x / kTerrainRegionSize));
		const auto regionY = static_cast<std::int32_t>(std::floor(a_position.y / kTerrainRegionSize));
		const auto found = _regionIndex.find(RegionKey(regionX, regionY));
		if (found == _regionIndex.end()) {
			return result;
		}
		const auto& region = _terrain.regions[found->second];
		if (region.layerRefCount == 0 || region.weightSampleSide < 2) {
			return result;
		}
		const Vec2 regionLocal{ a_position.x - regionX * kTerrainRegionSize, a_position.y - regionY * kTerrainRegionSize };
		const Vec2 dpdx{ a_texelWorldSize, 0.0f };
		const Vec2 dpdy{ 0.0f, a_texelWorldSize };

		const bool stochasticDiffuse = _settings.stochastic && _settings.stochasticDiffuse;
		const bool stochasticNormal = _settings.stochastic && _settings.stochasticNormal;
		const bool gaussian = _settings.stochastic && _settings.gaussian;
		const bool derivativeNormals = stochasticNormal && _settings.derivativeNormals;

		Vec3 baseColor{};
		Vec2 normalDerivative{};
		float roughness = 0.0f, metallic = 0.0f, occlusion = 0.0f, weightSum = 0.0f;
		for (std::uint32_t localLayer = 0; localLayer < region.layerRefCount; ++localLayer) {
			const float weight = LayerWeight(region, localLayer, regionLocal);
			if (weight <= 0.0001f) continue;
			const auto refIndex = region.layerRefStart + localLayer;
			if (refIndex >= _terrain.layerRefs.size() || _layers.empty()) continue;
			const auto& layer = _layers[(std::min<std::size_t>)(_terrain.layerRefs[refIndex].layerIndex, _layers.size() - 1)];
			weightSum += weight;

			const Vec2 uv = Scale(a_position, layer.uvScale);
			const Vec2 duDx = Scale(dpdx, layer.uvScale);
			const Vec2 duDy = Scale(dpdy, layer.uvScale);
			const bool hasStochasticLayer = layer.diffuseStochastic != nullptr || layer.normalStochastic != nullptr;

			// TerrainTryBuildLayerStochasticContext: the footprint's level from the diffuse, else the normal.
			const MipChain* lodTexture = layer.diffuse ? layer.diffuse.get() : layer.normal.get();
			const bool wantsContext = (stochasticDiffuse && layer.diffuse) || (gaussian && (stochasticDiffuse || stochasticNormal) && hasStochasticLayer) ||
			                          (derivativeNormals && layer.normal);
			StochasticContext ctx;
			const bool hasContext = wantsContext && lodTexture;
			if (hasContext) {
				ctx = TerrainBuildStochasticContext(uv, duDx, duDy, layer.stochasticScale, _settings.blendCurve, EstimateLod(*lodTexture, duDx, duDy));
			}

			Vec3 layerColor{ layer.fallbackColor.x, layer.fallbackColor.y, layer.fallbackColor.z };
			if (stochasticDiffuse && !gaussian && hasContext && layer.diffuse) {
				layerColor = TerrainSampleMikkelsenDiffuse(*layer.diffuse, ctx);
			} else if (stochasticDiffuse && gaussian && hasContext && layer.diffuseStochastic && !layer.diffuseStochastic->gaussian.Empty()) {
				layerColor = TerrainSampleStochasticDiffuse(*layer.diffuseStochastic, ctx);
			} else if (layer.diffuse) {
				const auto sample = Sample(*layer.diffuse, uv, duDx, duDy);
				layerColor = { sample.x, sample.y, sample.z };
			}
			baseColor = baseColor + layerColor * weight;

			float layerRoughness = 0.9f, layerMetallic = 0.0f, layerOcclusion = 1.0f;
			if ((layer.flags & kTerrainLayerPbr) != 0 && layer.rmaos) {
				const auto rmaos = Sample(*layer.rmaos, uv, duDx, duDy);
				layerRoughness = std::clamp(rmaos.x * (std::max)(layer.roughnessScale, 0.0f), 0.04f, 1.0f);
				layerMetallic = Saturate(rmaos.y);
				layerOcclusion = Saturate(rmaos.z);
			}
			roughness += layerRoughness * weight;
			metallic += layerMetallic * weight;
			occlusion += layerOcclusion * weight;

			Vec3 layerNormal{ 0.0f, 0.0f, 1.0f };
			if (derivativeNormals && hasContext && layer.normal) {
				layerNormal = TerrainSampleStochasticNormalDerivative(*layer.normal, ctx);
			} else if (stochasticNormal && gaussian && hasContext && layer.normalStochastic && !layer.normalStochastic->gaussian.Empty()) {
				layerNormal = TerrainSampleStochasticNormal(*layer.normalStochastic, ctx);
			} else if (layer.normal) {
				layerNormal = TerrainDecodeNormal(Sample(*layer.normal, uv, duDx, duDy));
			}
			const auto derivative = TerrainNormalToDerivative(layerNormal);
			normalDerivative = { normalDerivative.x + derivative.x * weight, normalDerivative.y + derivative.y * weight };
		}

		if (weightSum <= 1.0e-4f) {
			return result;
		}
		const float inverse = 1.0f / weightSum;
		result.albedo = baseColor * inverse;
		result.normal = TerrainDerivativeToNormal(Scale(normalDerivative, inverse));
		result.roughness = std::clamp(roughness * inverse, 0.04f, 1.0f);
		result.metallic = Saturate(metallic * inverse);
		result.ambientOcclusion = Saturate(occlusion * inverse);
		result.valid = true;
		return result;
	}

	Vec3 FromTerrainTangentSpace(Vec3 a_tangentNormal, Vec3 a_groundNormal)
	{
		const Vec3 normal = Normalize(a_groundNormal);
		Vec3 tangent = Cross({ 0.0f, 1.0f, 0.0f }, normal);
		if (Dot(tangent, tangent) < 1.0e-5f) {
			tangent = Cross({ 1.0f, 0.0f, 0.0f }, normal);
		}
		tangent = Normalize(tangent);
		const Vec3 bitangent = Normalize(Cross(normal, tangent));
		return Normalize(tangent * a_tangentNormal.x + bitangent * a_tangentNormal.y + normal * a_tangentNormal.z);
	}
}
