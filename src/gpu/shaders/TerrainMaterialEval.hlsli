// The painted landscape's material on the GPU: tesworld's TerrainMaterial::Evaluate (TerrainMaterial.cpp) line for line,
// itself a port of BasicRenderer's terrainCommon.hlsli (ApplyTerrainMaterialInternal and the functions it calls, whose
// names these keep). Albedo and the tangent-space normal only: the LOD bake reads nothing else.
#ifndef TESWORLD_TERRAIN_MATERIAL_EVAL_HLSLI
#define TESWORLD_TERRAIN_MATERIAL_EVAL_HLSLI

#include "TerrainSampling.hlsli"

static const float kTerrainRegionSize = 2048.0f;

struct StochasticContext
{
	float3 weights;
	float2 offsets0;
	float2 offsets1;
	float2 offsets2;
	float2 uv;
	float2 duDx;
	float2 duDy;
	float lod;
};

struct TerrainMaterialSample
{
	float3 albedo;
	float3 normal;
	bool valid;
};

float3 TerrainNormalize(float3 a_value)
{
	precise float length = sqrt(a_value.x * a_value.x + a_value.y * a_value.y + a_value.z * a_value.z);
	precise float3 result = length > 1.0e-12f ? a_value * (1.0f / length) : float3(0.0f, 0.0f, 1.0f);
	return result;
}

float3 TerrainCross(float3 a, float3 b)
{
	precise float3 result = float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
	return result;
}

uint TerrainHash(uint a_x, uint a_y)
{
	uint x = a_x * 1664525u + 1013904223u;
	uint y = a_y * 1664525u + 1013904223u;
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

float2 TerrainHash2(int a_x, int a_y)
{
	const uint h0 = TerrainHash(asuint(a_x), asuint(a_y));
	const uint h1 = TerrainHash(asuint(a_x + 37), asuint(a_y + 119));
	// A power of two: the product is the CPU's quotient exactly.
	return float2(float(h0 & 0x00ffffffu), float(h1 & 0x00ffffffu)) * (1.0f / 16777216.0f);
}

float TerrainBias(float a_value, float a_bias)
{
	a_value = saturate(a_value);
	a_bias = clamp(a_bias, 0.001f, 0.999f);
	precise float result = a_value / (((1.0f / a_bias) - 2.0f) * (1.0f - a_value) + 1.0f);
	return result;
}

float TerrainGain(float a_value, float a_gain)
{
	a_value = saturate(a_value);
	a_gain = saturate(a_gain);
	if (abs(a_gain - 0.5f) < 1.0e-4f) {
		return a_value;
	}
	return a_value < 0.5f ? 0.5f * TerrainBias(a_value * 2.0f, 1.0f - a_gain) : 1.0f - 0.5f * TerrainBias(2.0f - a_value * 2.0f, 1.0f - a_gain);
}

float3 TerrainNormalizeWeights(float3 a_weights)
{
	a_weights = max(a_weights, 0.0f.xxx);
	precise float sum = a_weights.x + a_weights.y + a_weights.z;
	precise float3 result = sum > 1.0e-5f ? a_weights * (1.0f / sum) : float3(1.0f, 0.0f, 0.0f);
	return result;
}

float3 TerrainShapeStochasticWeights(float3 a_weights, float a_blendCurve)
{
	a_blendCurve = saturate(a_blendCurve);
	precise float sharp = saturate((a_blendCurve - 0.5f) * 2.0f);
	precise float soft = saturate((0.5f - a_blendCurve) * 2.0f);
	precise float gamma = 1.0f + (7.0f - 1.0f) * sharp;
	gamma = gamma + (0.5f - gamma) * soft;
	float3 shaped = TerrainNormalizeWeights(float3(
		pow(max(a_weights.x, 1.0e-5f), gamma),
		pow(max(a_weights.y, 1.0e-5f), gamma),
		pow(max(a_weights.z, 1.0e-5f), gamma)));
	shaped = float3(TerrainGain(shaped.x, a_blendCurve), TerrainGain(shaped.y, a_blendCurve), TerrainGain(shaped.z, a_blendCurve));
	return TerrainNormalizeWeights(shaped);
}

StochasticContext TerrainBuildStochasticContext(float2 a_uv, float2 a_duDx, float2 a_duDy, float a_scale, float a_blendCurve, float a_lod)
{
	StochasticContext ctx;
	ctx.uv = a_uv;
	ctx.duDx = a_duDx;
	ctx.duDy = a_duDy;
	ctx.lod = max(a_lod, 0.0f);
	const float scale = max(a_scale, 0.001f);
	precise float2 grid = a_uv * scale;
	precise float2 skewed = float2(grid.x - 0.57735027f * grid.y, 1.15470054f * grid.y);
	const int baseX = int(floor(skewed.x));
	const int baseY = int(floor(skewed.y));
	precise float fx = skewed.x - float(baseX);
	precise float fy = skewed.y - float(baseY);
	precise float z = 1.0f - fx - fy;
	float3 w;
	if (z > 0.0f) {
		ctx.offsets0 = TerrainHash2(baseX, baseY);
		ctx.offsets1 = TerrainHash2(baseX, baseY + 1);
		ctx.offsets2 = TerrainHash2(baseX + 1, baseY);
		w = float3(z, fy, fx);
	} else {
		ctx.offsets0 = TerrainHash2(baseX + 1, baseY + 1);
		ctx.offsets1 = TerrainHash2(baseX + 1, baseY);
		ctx.offsets2 = TerrainHash2(baseX, baseY + 1);
		w = float3(-z, 1.0f - fy, 1.0f - fx);
	}
	ctx.weights = TerrainShapeStochasticWeights(w, a_blendCurve);
	return ctx;
}

float VarianceScale(float3 a_weights)
{
	precise float dotWeights = a_weights.x * a_weights.x + a_weights.y * a_weights.y + a_weights.z * a_weights.z;
	return 1.0f / sqrt(max(dotWeights, 1.0e-4f));
}

float Blend1(float a, float b, float c, float3 w)
{
	precise float result = saturate(((a - 0.5f) * w.x + (b - 0.5f) * w.y + (c - 0.5f) * w.z) * VarianceScale(w) + 0.5f);
	return result;
}

float LutY(float a_lod, float a_lutHeight)
{
	return saturate((max(a_lod, 0.0f) + 0.5f) / max(a_lutHeight, 1.0f));
}

float4 InverseLut(ByteAddressBuffer a_material, MaterialHeader a_header, StochasticGpu a_texture, float a_gaussian, float a_lod)
{
	return SampleBilinear(LoadLevel(a_material, a_header, a_texture.lut), float2(a_gaussian, LutY(a_lod, float(a_texture.lutHeight))), false);
}

StochasticGpu LoadStochastic(ByteAddressBuffer a_material, MaterialHeader a_header, uint a_index)
{
	return a_material.Load<StochasticGpu>(a_header.stochasticOffset + a_index * 64u);
}

float3 TerrainSampleStochasticDiffuse(ByteAddressBuffer a_material, MaterialHeader a_header, StochasticGpu a_texture, StochasticContext ctx)
{
	const float4 g0 = SampleGrad(a_material, a_header, a_texture.gaussian, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy);
	const float4 g1 = SampleGrad(a_material, a_header, a_texture.gaussian, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy);
	const float4 g2 = SampleGrad(a_material, a_header, a_texture.gaussian, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy);
	precise float3 color = float3(
		InverseLut(a_material, a_header, a_texture, Blend1(g0.x, g1.x, g2.x, ctx.weights), ctx.lod).x,
		InverseLut(a_material, a_header, a_texture, Blend1(g0.y, g1.y, g2.y, ctx.weights), ctx.lod).y,
		InverseLut(a_material, a_header, a_texture, Blend1(g0.z, g1.z, g2.z, ctx.weights), ctx.lod).z);
	if (a_texture.mode == kStochasticDecorrelatedColor) {
		precise float3 decorrelated = float3(a_texture.originX, a_texture.originY, a_texture.originZ) + float3(a_texture.v0X, a_texture.v0Y, a_texture.v0Z) * color.x +
		                              float3(a_texture.v1X, a_texture.v1Y, a_texture.v1Z) * color.y + float3(a_texture.v2X, a_texture.v2Y, a_texture.v2Z) * color.z;
		color = decorrelated;
	}
	return saturate(color);
}

float3 TerrainSampleMikkelsenDiffuse(ByteAddressBuffer a_material, MaterialHeader a_header, uint a_texture, StochasticContext ctx)
{
	const float4 c0 = SampleGrad(a_material, a_header, a_texture, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy);
	const float4 c1 = SampleGrad(a_material, a_header, a_texture, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy);
	const float4 c2 = SampleGrad(a_material, a_header, a_texture, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy);
	const float3 w = ctx.weights;
	precise float3 result = saturate(c0.xyz * w.x + c1.xyz * w.y + c2.xyz * w.z);
	return result;
}

float3 TerrainDecodeNormal(float4 a_encoded)
{
	precise float3 decoded = float3(a_encoded.x * 2.0f - 1.0f, a_encoded.y * 2.0f - 1.0f, a_encoded.z * 2.0f - 1.0f);
	return TerrainNormalize(decoded);
}

float3 TerrainSampleStochasticNormal(ByteAddressBuffer a_material, MaterialHeader a_header, StochasticGpu a_texture, StochasticContext ctx)
{
	const float4 g0 = SampleGrad(a_material, a_header, a_texture.gaussian, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy);
	const float4 g1 = SampleGrad(a_material, a_header, a_texture.gaussian, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy);
	const float4 g2 = SampleGrad(a_material, a_header, a_texture.gaussian, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy);
	precise float x = InverseLut(a_material, a_header, a_texture, Blend1(g0.x, g1.x, g2.x, ctx.weights), ctx.lod).x * 2.0f - 1.0f;
	precise float y = InverseLut(a_material, a_header, a_texture, Blend1(g0.y, g1.y, g2.y, ctx.weights), ctx.lod).y * 2.0f - 1.0f;
	return TerrainNormalize(float3(x, y, sqrt(saturate(1.0f - x * x - y * y))));
}

float2 TerrainNormalToDerivative(float3 a_normal)
{
	a_normal = TerrainNormalize(a_normal);
	const float nz = max(a_normal.z, 0.05f);
	return float2(-a_normal.x / nz, -a_normal.y / nz);
}

float3 TerrainDerivativeToNormal(float2 a_derivative)
{
	return TerrainNormalize(float3(-a_derivative.x, -a_derivative.y, 1.0f));
}

float3 TerrainSampleStochasticNormalDerivative(ByteAddressBuffer a_material, MaterialHeader a_header, uint a_texture, StochasticContext ctx)
{
	const float2 d0 = TerrainNormalToDerivative(TerrainDecodeNormal(SampleGrad(a_material, a_header, a_texture, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy)));
	const float2 d1 = TerrainNormalToDerivative(TerrainDecodeNormal(SampleGrad(a_material, a_header, a_texture, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy)));
	const float2 d2 = TerrainNormalToDerivative(TerrainDecodeNormal(SampleGrad(a_material, a_header, a_texture, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy)));
	const float3 w = ctx.weights;
	precise float2 blended = float2(d0.x * w.x + d1.x * w.y + d2.x * w.z, d0.y * w.x + d1.y * w.y + d2.y * w.z);
	return TerrainDerivativeToNormal(blended);
}

float TerrainCubicBSpline(float p0, float p1, float p2, float p3, float t)
{
	precise float t2 = t * t;
	precise float t3 = t2 * t;
	precise float w0 = (1.0f - 3.0f * t + 3.0f * t2 - t3) * (1.0f / 6.0f);
	precise float w1 = (4.0f - 6.0f * t2 + 3.0f * t3) * (1.0f / 6.0f);
	precise float w2 = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) * (1.0f / 6.0f);
	precise float w3 = t3 * (1.0f / 6.0f);
	precise float result = p0 * w0 + p1 * w1 + p2 * w2 + p3 * w3;
	return result;
}

float TerrainLoadWeight(ByteAddressBuffer a_material, MaterialHeader a_header, uint a_layerWordBase, int a_side, int a_x, int a_y)
{
	const int maxSample = a_side - 2;
	const uint x = uint(clamp(a_x, -1, maxSample) + 1);
	const uint y = uint(clamp(a_y, -1, maxSample) + 1);
	const uint sample = y * uint(a_side) + x;
	const uint word = a_layerWordBase + sample / 4u;
	if (word >= a_header.weightCount) {
		return 0.0f;
	}
	return float((a_material.Load(a_header.weightsOffset + word * 4u) >> ((sample & 3u) * 8u)) & 0xFFu) / 255.0f;
}

// TerrainInterpolateLayerWeight: the 17x17 lattice (with its one-sample gutter) through a cubic B-spline.
float TerrainLayerWeight(ByteAddressBuffer a_material, MaterialHeader a_header, RegionGpu a_region, uint a_localLayer, float2 a_regionLocal)
{
	// A power of two: the product is the CPU's quotient exactly.
	precise float gridX = saturate(a_regionLocal.x * (1.0f / kTerrainRegionSize)) * 16.0f;
	precise float gridY = saturate(a_regionLocal.y * (1.0f / kTerrainRegionSize)) * 16.0f;
	const int baseX = int(floor(gridX));
	const int baseY = int(floor(gridY));
	precise float fx = gridX - float(baseX);
	precise float fy = gridY - float(baseY);
	const int side = int(a_region.weightSampleSide);
	const uint samplesPerLayer = uint(side) * uint(side);
	const uint wordsPerLayer = (samplesPerLayer + 3u) / 4u;
	const uint layerWordBase = a_region.weightBlockStart + a_localLayer * wordsPerLayer;
	float rows[4];
	[unroll]
	for (int r = 0; r < 4; ++r) {
		const int y = baseY - 1 + r;
		rows[r] = TerrainCubicBSpline(
			TerrainLoadWeight(a_material, a_header, layerWordBase, side, baseX - 1, y),
			TerrainLoadWeight(a_material, a_header, layerWordBase, side, baseX, y),
			TerrainLoadWeight(a_material, a_header, layerWordBase, side, baseX + 1, y),
			TerrainLoadWeight(a_material, a_header, layerWordBase, side, baseX + 2, y),
			fx);
	}
	return saturate(TerrainCubicBSpline(rows[0], rows[1], rows[2], rows[3], fy));
}

TerrainMaterialSample TerrainEvaluate(ByteAddressBuffer a_material, float2 a_position, float a_texelWorldSize)
{
	TerrainMaterialSample result;
	result.albedo = float3(0.0f, 0.0f, 0.0f);
	result.normal = float3(0.0f, 0.0f, 1.0f);
	result.valid = false;

	const MaterialHeader header = a_material.Load<MaterialHeader>(0u);
	const int regionX = int(floor(a_position.x * (1.0f / kTerrainRegionSize)));
	const int regionY = int(floor(a_position.y * (1.0f / kTerrainRegionSize)));
	const int gridX = regionX - header.rx0;
	const int gridY = regionY - header.ry0;
	if (gridX < 0 || gridY < 0 || gridX >= int(header.gridWidth) || gridY >= int(header.gridHeight)) {
		return result;
	}
	const uint entry = a_material.Load(header.gridOffset + (uint(gridY) * header.gridWidth + uint(gridX)) * 4u);
	if (entry == 0u) {
		return result;
	}
	const RegionGpu region = a_material.Load<RegionGpu>(header.regionsOffset + (entry - 1u) * 16u);
	if (region.layerRefCount == 0u || region.weightSampleSide < 2u) {
		return result;
	}
	precise float2 regionLocal = float2(a_position.x - float(regionX) * kTerrainRegionSize, a_position.y - float(regionY) * kTerrainRegionSize);
	const float2 dpdx = float2(a_texelWorldSize, 0.0f);
	const float2 dpdy = float2(0.0f, a_texelWorldSize);

	const bool stochasticDiffuse = (header.flags & kMaterialStochasticDiffuse) != 0u;
	const bool stochasticNormal = (header.flags & kMaterialStochasticNormal) != 0u;
	const bool gaussian = (header.flags & kMaterialGaussian) != 0u;
	const bool derivativeNormals = (header.flags & kMaterialDerivativeNormals) != 0u;

	precise float3 baseColor = float3(0.0f, 0.0f, 0.0f);
	precise float2 normalDerivative = float2(0.0f, 0.0f);
	precise float weightSum = 0.0f;
	for (uint localLayer = 0u; localLayer < region.layerRefCount; ++localLayer) {
		const float weight = TerrainLayerWeight(a_material, header, region, localLayer, regionLocal);
		if (weight <= 0.0001f) {
			continue;
		}
		const uint refIndex = region.layerRefStart + localLayer;
		if (refIndex >= header.layerRefCount || header.layerCount == 0u) {
			continue;
		}
		const uint layerIndex = min(a_material.Load(header.layerRefsOffset + refIndex * 4u), header.layerCount - 1u);
		const LayerGpu layer = a_material.Load<LayerGpu>(header.layersOffset + layerIndex * 48u);
		weightSum += weight;

		precise float2 uv = a_position * layer.uvScale;
		precise float2 duDx = dpdx * layer.uvScale;
		precise float2 duDy = dpdy * layer.uvScale;
		const bool hasDiffuse = layer.diffuse != kNone;
		const bool hasNormal = layer.normal != kNone;
		const bool hasStochasticLayer = layer.diffuseStochastic != kNone || layer.normalStochastic != kNone;

		// TerrainTryBuildLayerStochasticContext: the footprint's level from the diffuse, else the normal.
		const uint lodTexture = hasDiffuse ? layer.diffuse : layer.normal;
		const bool wantsContext = (stochasticDiffuse && hasDiffuse) || (gaussian && (stochasticDiffuse || stochasticNormal) && hasStochasticLayer) ||
		                          (derivativeNormals && hasNormal);
		const bool hasContext = wantsContext && lodTexture != kNone;
		StochasticContext ctx = (StochasticContext)0;
		ctx.weights = float3(1.0f, 0.0f, 0.0f);
		if (hasContext) {
			ctx = TerrainBuildStochasticContext(uv, duDx, duDy, layer.stochasticScale, header.blendCurve,
				EstimateLod(LoadTexture(a_material, header, lodTexture), duDx, duDy));
		}

		float3 layerColor = float3(layer.fallbackR, layer.fallbackG, layer.fallbackB);
		bool stochasticDiffuseUsable = false;
		StochasticGpu diffuseStochastic = (StochasticGpu)0;
		if (layer.diffuseStochastic != kNone) {
			diffuseStochastic = LoadStochastic(a_material, header, layer.diffuseStochastic);
			stochasticDiffuseUsable = LoadTexture(a_material, header, diffuseStochastic.gaussian).levelCount > 0u;
		}
		if (stochasticDiffuse && !gaussian && hasContext && hasDiffuse) {
			layerColor = TerrainSampleMikkelsenDiffuse(a_material, header, layer.diffuse, ctx);
		} else if (stochasticDiffuse && gaussian && hasContext && stochasticDiffuseUsable) {
			layerColor = TerrainSampleStochasticDiffuse(a_material, header, diffuseStochastic, ctx);
		} else if (hasDiffuse) {
			layerColor = SampleGrad(a_material, header, layer.diffuse, uv, duDx, duDy).xyz;
		}
		baseColor = baseColor + layerColor * weight;

		float3 layerNormal = float3(0.0f, 0.0f, 1.0f);
		bool stochasticNormalUsable = false;
		StochasticGpu normalStochastic = (StochasticGpu)0;
		if (layer.normalStochastic != kNone) {
			normalStochastic = LoadStochastic(a_material, header, layer.normalStochastic);
			stochasticNormalUsable = LoadTexture(a_material, header, normalStochastic.gaussian).levelCount > 0u;
		}
		if (derivativeNormals && hasContext && hasNormal) {
			layerNormal = TerrainSampleStochasticNormalDerivative(a_material, header, layer.normal, ctx);
		} else if (stochasticNormal && gaussian && hasContext && stochasticNormalUsable) {
			layerNormal = TerrainSampleStochasticNormal(a_material, header, normalStochastic, ctx);
		} else if (hasNormal) {
			layerNormal = TerrainDecodeNormal(SampleGrad(a_material, header, layer.normal, uv, duDx, duDy));
		}
		const float2 derivative = TerrainNormalToDerivative(layerNormal);
		normalDerivative = float2(normalDerivative.x + derivative.x * weight, normalDerivative.y + derivative.y * weight);
	}

	if (weightSum <= 1.0e-4f) {
		return result;
	}
	const float inverse = 1.0f / weightSum;
	result.albedo = baseColor * inverse;
	result.normal = TerrainDerivativeToNormal(normalDerivative * inverse);
	result.valid = true;
	return result;
}

// BasicRenderer's TerrainBasis: tangent = north x normal, bitangent = normal x tangent; the tangent-space normal turned
// onto a ground normal (tesworld's FromTerrainTangentSpace).
float3 FromTerrainTangentSpace(float3 a_tangentNormal, float3 a_groundNormal)
{
	const float3 normal = TerrainNormalize(a_groundNormal);
	float3 tangent = TerrainCross(float3(0.0f, 1.0f, 0.0f), normal);
	if (dot(tangent, tangent) < 1.0e-5f) {
		tangent = TerrainCross(float3(1.0f, 0.0f, 0.0f), normal);
	}
	tangent = TerrainNormalize(tangent);
	const float3 bitangent = TerrainNormalize(TerrainCross(normal, tangent));
	precise float3 turned = tangent * a_tangentNormal.x + bitangent * a_tangentNormal.y + normal * a_tangentNormal.z;
	return TerrainNormalize(turned);
}

#endif
