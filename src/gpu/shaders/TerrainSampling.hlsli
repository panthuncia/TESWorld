// The terrain material's textures sampled as tesworld's CPU samples them (Texture.cpp: SampleLevel, EstimateLod), their
// levels in the material's tables (Sampling.hlsli filters one).
#ifndef TESWORLD_TERRAIN_SAMPLING_HLSLI
#define TESWORLD_TERRAIN_SAMPLING_HLSLI

#include "Sampling.hlsli"

TextureGpu LoadTexture(ByteAddressBuffer a_material, MaterialHeader a_header, uint a_texture)
{
	return a_material.Load<TextureGpu>(a_header.texturesOffset + a_texture * 16u);
}

LevelGpu LoadLevel(ByteAddressBuffer a_material, MaterialHeader a_header, uint a_level)
{
	return a_material.Load<LevelGpu>(a_header.levelsOffset + a_level * 16u);
}

float EstimateLod(TextureGpu a_texture, float2 a_duDx, float2 a_duDy)
{
	const float width = float(max(1u, a_texture.width));
	const float height = float(max(1u, a_texture.height));
	precise float dx = (a_duDx.x * width) * (a_duDx.x * width) + (a_duDx.y * height) * (a_duDx.y * height);
	precise float dy = (a_duDy.x * width) * (a_duDy.x * width) + (a_duDy.y * height) * (a_duDy.y * height);
	return max(0.0f, 0.5f * log2(max(max(dx, dy), 1.0e-8f)));
}

float4 SampleLevel(ByteAddressBuffer a_material, MaterialHeader a_header, TextureGpu a_texture, float2 a_uv, float a_lod, bool a_wrap)
{
	if (a_texture.levelCount == 0u) {
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	const float maxLevel = float(a_texture.levelCount - 1u);
	const float lod = clamp(a_lod, 0.0f, maxLevel);
	const uint low = uint(floor(lod));
	const uint high = min(low + 1u, a_texture.levelCount - 1u);
	precise float t = lod - float(low);
	const float4 a = SampleBilinear(LoadLevel(a_material, a_header, a_texture.firstLevel + low), a_uv, a_wrap);
	if (t <= 0.0f || high == low) {
		return a;
	}
	const float4 b = SampleBilinear(LoadLevel(a_material, a_header, a_texture.firstLevel + high), a_uv, a_wrap);
	precise float4 result = a + (b - a) * t;
	return result;
}

float4 SampleGrad(ByteAddressBuffer a_material, MaterialHeader a_header, uint a_texture, float2 a_uv, float2 a_duDx, float2 a_duDy)
{
	const TextureGpu texture = LoadTexture(a_material, a_header, a_texture);
	return SampleLevel(a_material, a_header, texture, a_uv, EstimateLod(texture, a_duDx, a_duDy), true);
}

#endif
