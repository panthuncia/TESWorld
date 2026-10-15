// Texture sampling as tesworld's CPU samples (Texture.cpp: SampleBilinear): textures are RGBA8 in pool buffers and
// filtered in shader arithmetic, not by the hardware's samplers, so a texel weighs what it weighs on the CPU. `precise`
// keeps the compiler from fusing multiply-adds the CPU doesn't fuse. Mip chains are each user's (TerrainSampling.hlsli,
// View.hlsl).
#ifndef TESWORLD_SAMPLING_HLSLI
#define TESWORLD_SAMPLING_HLSLI

#include "Layout.hlsli"

float4 UnpackRgba8(uint a_value)
{
	precise float4 texel = float4(a_value & 0xFFu, (a_value >> 8u) & 0xFFu, (a_value >> 16u) & 0xFFu, a_value >> 24u) * (1.0f / 255.0f);
	return texel;
}

uint WrapCoord(int a_value, uint a_size)
{
	const int size = int(a_size);
	const int wrapped = a_value % size;
	return uint(wrapped < 0 ? wrapped + size : wrapped);
}

uint ClampCoord(int a_value, uint a_size)
{
	return uint(clamp(a_value, 0, int(a_size) - 1));
}

float4 LevelTexel(LevelGpu a_level, uint a_x, uint a_y)
{
	ByteAddressBuffer pool = ResourceDescriptorHeap[NonUniformResourceIndex(a_level.pool)];
	return UnpackRgba8(pool.Load(a_level.offset + (a_y * a_level.width + a_x) * 4u));
}

float4 SampleBilinear(LevelGpu a_level, float2 a_uv, bool a_wrap)
{
	if (a_level.width == 0u || a_level.height == 0u) {
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	precise float fx = a_uv.x * float(a_level.width) - 0.5f;
	precise float fy = a_uv.y * float(a_level.height) - 0.5f;
	const int x0 = int(floor(fx));
	const int y0 = int(floor(fy));
	precise float tx = fx - float(x0);
	precise float ty = fy - float(y0);
	const uint ax0 = a_wrap ? WrapCoord(x0, a_level.width) : ClampCoord(x0, a_level.width);
	const uint ax1 = a_wrap ? WrapCoord(x0 + 1, a_level.width) : ClampCoord(x0 + 1, a_level.width);
	const uint ay0 = a_wrap ? WrapCoord(y0, a_level.height) : ClampCoord(y0, a_level.height);
	const uint ay1 = a_wrap ? WrapCoord(y0 + 1, a_level.height) : ClampCoord(y0 + 1, a_level.height);
	const float4 t00 = LevelTexel(a_level, ax0, ay0);
	const float4 t10 = LevelTexel(a_level, ax1, ay0);
	const float4 t01 = LevelTexel(a_level, ax0, ay1);
	const float4 t11 = LevelTexel(a_level, ax1, ay1);
	precise float4 top = t00 + (t10 - t00) * tx;
	precise float4 bottom = t01 + (t11 - t01) * tx;
	precise float4 result = top + (bottom - top) * ty;
	return result;
}

#endif
