// One view's samples box-filtered down as CpuRenderer filters them (Render.cpp): each pixel's colour the mean of its
// covered samples, its alpha how many are; its normal the mean's direction in the camera's frame, encoded as a normal
// map's. Uncovered pixels are 0. Bytes are rounded as Image::SetTexel rounds them.
#include "Layout.hlsli"

cbuffer Push : register(b0)
{
	ViewResolveConstants g;
};

uint ToByte(float a_value)
{
	return uint(floor(saturate(a_value) * 255.0f + 0.5f));
}

uint Pack(float4 a_value)
{
	return ToByte(a_value.x) | (ToByte(a_value.y) << 8u) | (ToByte(a_value.z) << 16u) | (ToByte(a_value.w) << 24u);
}

[numthreads(8, 8, 1)]
void ResolveCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g.width || id.y >= g.height) {
		return;
	}
	Texture2D<float4> colors = ResourceDescriptorHeap[g.color];
	Texture2D<float4> normals = ResourceDescriptorHeap[g.normal];
	RWByteAddressBuffer output = ResourceDescriptorHeap[g.output];
	float3 sum = float3(0.0f, 0.0f, 0.0f);
	float3 normalSum = float3(0.0f, 0.0f, 0.0f);
	uint count = 0u;
	for (uint sy = 0u; sy < g.supersample; ++sy) {
		for (uint sx = 0u; sx < g.supersample; ++sx) {
			const int3 at = int3(g.x + id.x * g.supersample + sx, g.y + id.y * g.supersample + sy, 0);
			const float4 color = colors.Load(at);
			if (color.w <= 0.0f) {
				continue;
			}
			sum += color.xyz;
			normalSum += normals.Load(at).xyz;
			++count;
		}
	}
	uint colorWord = 0u, normalWord = 0u;
	if (count > 0u) {
		const float inverse = 1.0f / float(count);
		const float alpha = float(count) / float(g.supersample * g.supersample);
		colorWord = Pack(float4(min(1.0f, sum.x * inverse), min(1.0f, sum.y * inverse), min(1.0f, sum.z * inverse), alpha));
		const float3 right = float3(g.rightX, g.rightY, g.rightZ);
		const float3 up = float3(g.upX, g.upY, g.upZ);
		const float3 toCamera = -float3(g.forwardX, g.forwardY, g.forwardZ);
		const float length = sqrt(dot(normalSum, normalSum));
		const float3 n = length > 1e-12f ? normalSum * (1.0f / length) : toCamera;
		const float3 view = float3(dot(n, right), dot(n, up), dot(n, toCamera));
		normalWord = Pack(float4(view * 0.5f + 0.5f, alpha));
	}
	const uint texel = (id.y * g.width + id.x) * 4u;
	output.Store(g.colorOffset + texel, colorWord);
	output.Store(g.normalOffset + texel, normalWord);
}
