// A square of terrain's LOD textures (tesworld's BakeTerrainImages, TerrainBake.cpp), a thread a texel: the material's
// colour tinted by LAND's vertex colours and brightened, and its normal turned onto the landscape's and encoded as
// Skyrim's terrain LOD normal maps are. With both images the same size, the material is evaluated once for the two.
#include "TerrainMaterialEval.hlsli"

cbuffer Push : register(b0)
{
	BakeConstants g;
};

static const float kLandVertexSpacing = 128.0f;

float LatticeHeight(ByteAddressBuffer a_world, WorldHeader a_header, int a_gx, int a_gy)
{
	const int x = a_gx - a_header.gx0;
	const int y = a_gy - a_header.gy0;
	if (x < 0 || y < 0 || x >= int(a_header.width) || y >= int(a_header.height)) {
		return a_header.defaultHeight;
	}
	return asfloat(a_world.Load(a_header.heightsOffset + (uint(y) * a_header.width + uint(x)) * 4u));
}

float3 LatticeColor(ByteAddressBuffer a_world, WorldHeader a_header, int a_gx, int a_gy)
{
	const int x = a_gx - a_header.gx0;
	const int y = a_gy - a_header.gy0;
	if (x < 0 || y < 0 || x >= int(a_header.width) || y >= int(a_header.height)) {
		return float3(1.0f, 1.0f, 1.0f);
	}
	const uint packed = a_world.Load(a_header.colorsOffset + (uint(y) * a_header.width + uint(x)) * 4u);
	return float3(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu) / 255.0f;
}

// WorldTerrain::NormalAt: from the heights around the point.
float3 LatticeNormal(ByteAddressBuffer a_world, WorldHeader a_header, int a_gx, int a_gy)
{
	precise float dx = LatticeHeight(a_world, a_header, a_gx + 1, a_gy) - LatticeHeight(a_world, a_header, a_gx - 1, a_gy);
	precise float dy = LatticeHeight(a_world, a_header, a_gx, a_gy + 1) - LatticeHeight(a_world, a_header, a_gx, a_gy - 1);
	return TerrainNormalize(float3(-dx, -dy, 2.0f * kLandVertexSpacing));
}

void LatticeCell(float2 a_position, out int2 a_base, out float2 a_t)
{
	// A power of two: the product is the CPU's quotient exactly.
	precise float2 f = a_position * (1.0f / kLandVertexSpacing);
	a_base = int2(floor(f));
	a_t = f - float2(a_base);
}

float3 Bilinear(float3 a, float3 b, float3 c, float3 d, float2 t)
{
	precise float3 result = (a * (1 - t.x) + b * t.x) * (1 - t.y) + (c * (1 - t.x) + d * t.x) * t.y;
	return result;
}

uint ToByte(float a_value)
{
	precise float scaled = saturate(a_value) * 255.0f + 0.5f;
	return min(uint(floor(scaled)), 255u);
}

uint PackRgba8(float4 a_value)
{
	return ToByte(a_value.x) | (ToByte(a_value.y) << 8u) | (ToByte(a_value.z) << 16u) | (ToByte(a_value.w) << 24u);
}

[numthreads(8, 8, 1)]
void BakeCS(uint3 id : SV_DispatchThreadID)
{
	const bool diffuse = (g.flags & kBakeDiffuse) != 0u;
	const bool normal = (g.flags & kBakeNormal) != 0u;
	const uint size = diffuse ? g.diffuseSize : g.normalSize;
	if (id.x >= size || id.y >= size) {
		return;
	}
	ByteAddressBuffer world = ResourceDescriptorHeap[g.world];
	ByteAddressBuffer material = ResourceDescriptorHeap[g.material];
	RWByteAddressBuffer output = ResourceDescriptorHeap[g.output];
	const WorldHeader worldHeader = world.Load<WorldHeader>(0u);

	const float texel = g.extent / float(size);
	// Row 0 is the square's north edge: the mesh's v runs 1 - y.
	precise float2 position = float2(g.originX + (float(id.x) + 0.5f) * texel, g.originY + g.extent - (float(id.y) + 0.5f) * texel);
	const TerrainMaterialSample sample = TerrainEvaluate(material, position, texel);
	int2 cell;
	float2 t;
	LatticeCell(position, cell, t);
	const uint texelOffset = (id.y * size + id.x) * 4u;

	if (diffuse) {
		precise float3 colour = sample.valid ? sample.albedo : float3(g.fallbackR, g.fallbackG, g.fallbackB);
		if ((g.flags & kBakeVertexColors) != 0u) {
			const float3 tint = Bilinear(
				LatticeColor(world, worldHeader, cell.x, cell.y), LatticeColor(world, worldHeader, cell.x + 1, cell.y),
				LatticeColor(world, worldHeader, cell.x, cell.y + 1), LatticeColor(world, worldHeader, cell.x + 1, cell.y + 1), t);
			colour = colour * tint;
		}
		colour = colour * g.gain + g.bias;
		output.Store(g.diffuseOffset + texelOffset, PackRgba8(float4(colour, 1.0f)));
	}
	if (normal) {
		const float3 ground = TerrainNormalize(Bilinear(
			LatticeNormal(world, worldHeader, cell.x, cell.y), LatticeNormal(world, worldHeader, cell.x + 1, cell.y),
			LatticeNormal(world, worldHeader, cell.x, cell.y + 1), LatticeNormal(world, worldHeader, cell.x + 1, cell.y + 1), t));
		const float3 turned = TerrainNormalize(sample.valid ? FromTerrainTangentSpace(sample.normal, ground) : ground);
		// Skyrim's terrain LOD normal maps: model space, green up.
		precise float4 encoded = float4(turned.x * 0.5f + 0.5f, turned.z * 0.5f + 0.5f, turned.y * 0.5f + 0.5f, 1.0f);
		output.Store(g.normalOffset + texelOffset, PackRgba8(encoded));
	}
}
