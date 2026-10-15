// One mip level from the one above, as tesworld's BuildMips makes it (HalfSize, Texture.cpp): each texel the rounded
// mean of its 2x2 source texels, clamped at the far edge, in integers, so the chain is byte for byte the CPU's.
#include "Layout.hlsli"

cbuffer Push : register(b0)
{
	HalfSizeConstants g;
};

[numthreads(8, 8, 1)]
void HalfSizeCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g.destinationWidth || id.y >= g.destinationHeight) {
		return;
	}
	RWByteAddressBuffer buffer = ResourceDescriptorHeap[g.buffer];
	uint4 sum = uint4(0u, 0u, 0u, 0u);
	[unroll]
	for (uint oy = 0u; oy < 2u; ++oy) {
		[unroll]
		for (uint ox = 0u; ox < 2u; ++ox) {
			const uint sx = min(g.sourceWidth - 1u, id.x * 2u + ox);
			const uint sy = min(g.sourceHeight - 1u, id.y * 2u + oy);
			const uint texel = buffer.Load(g.sourceOffset + (sy * g.sourceWidth + sx) * 4u);
			sum += uint4(texel & 0xFFu, (texel >> 8u) & 0xFFu, (texel >> 16u) & 0xFFu, texel >> 24u);
		}
	}
	const uint4 mean = (sum + 2u) / 4u;
	buffer.Store(g.destinationOffset + (id.y * g.destinationWidth + id.x) * 4u, mean.x | (mean.y << 8u) | (mean.z << 16u) | (mean.w << 24u));
}
