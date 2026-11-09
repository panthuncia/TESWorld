// BC4 (one channel: red) and BC5 (two: red, green), a thread a 4x4 block, reading RGBA8 from a buffer. Each channel is
// its own 8-byte block: two 8-bit endpoints and sixteen 3-bit indices. Both of the format's palettes are tried, and the
// one with the smaller squared error kept: endpoints first (six values between them), for smooth ranges; endpoints
// second (four between, plus exact 0 and 255), for blocks that touch the range's ends, as normal maps' do.
#include "Layout.hlsli"

cbuffer Push : register(b0)
{
	BcChannelsConstants g;
};

struct ChannelBlock
{
	uint2 words;
	float error;
};

float PaletteValue(uint e0, uint e1, uint index)
{
	const float a = (float)e0;
	const float b = (float)e1;
	if (e0 > e1) {
		// 0 and 1 are the endpoints; 2..7 step from e0 toward e1 in sevenths.
		return index == 0u ? a : index == 1u ? b : ((8.0f - (float)index) * a + ((float)index - 1.0f) * b) / 7.0f;
	}
	// 0 and 1 are the endpoints; 2..5 step in fifths; 6 is 0 and 7 is 255.
	if (index == 0u) return a;
	if (index == 1u) return b;
	if (index == 6u) return 0.0f;
	if (index == 7u) return 255.0f;
	return ((6.0f - (float)index) * a + ((float)index - 1.0f) * b) / 5.0f;
}

ChannelBlock EncodeWith(float values[16], uint e0, uint e1)
{
	ChannelBlock block;
	block.error = 0.0f;
	uint indexBits[2] = { 0u, 0u };  // 48 bits of indices, texel 0 lowest
	[unroll]
	for (uint texel = 0u; texel < 16u; ++texel) {
		uint best = 0u;
		float bestError = 1.0e30f;
		[unroll]
		for (uint index = 0u; index < 8u; ++index) {
			const float d = PaletteValue(e0, e1, index) - values[texel];
			if (d * d < bestError) {
				bestError = d * d;
				best = index;
			}
		}
		block.error += bestError;
		const uint bit = texel * 3u;
		if (bit < 32u) {
			indexBits[0] |= best << bit;
			if (bit > 29u) indexBits[1] |= best >> (32u - bit);
		}
		else {
			indexBits[1] |= best << (bit - 32u);
		}
	}
	// Byte 0 e0, byte 1 e1, then the indices.
	block.words.x = e0 | (e1 << 8u) | (indexBits[0] << 16u);
	block.words.y = (indexBits[0] >> 16u) | (indexBits[1] << 16u);
	return block;
}

ChannelBlock EncodeChannel(float values[16])
{
	float low = 255.0f;
	float high = 0.0f;
	// The range without exact ends, for the palette that has them already.
	float innerLow = 255.0f;
	float innerHigh = 0.0f;
	[unroll]
	for (uint texel = 0u; texel < 16u; ++texel) {
		low = min(low, values[texel]);
		high = max(high, values[texel]);
		if (values[texel] > 0.0f && values[texel] < 255.0f) {
			innerLow = min(innerLow, values[texel]);
			innerHigh = max(innerHigh, values[texel]);
		}
	}
	const uint lo = (uint)round(low);
	const uint hi = (uint)round(high);
	if (hi == lo) {
		// One value: every index 0.
		ChannelBlock flat;
		flat.words = uint2(hi | (hi << 8u), 0u);
		flat.error = 0.0f;
		return flat;
	}
	// Six values between: e0 > e1 selects that palette. The range's own ends are rarely its best endpoints: an outlier
	// stretches the steps between them, so endpoints a few steps in are searched too (about 1.6 dB on noisy normal maps,
	// as much as a wider search finds).
	ChannelBlock best = EncodeWith(values, hi, lo);
	[loop]
	for (uint inHigh = 0u; inHigh < 8u; ++inHigh) {
		[loop]
		for (uint inLow = 0u; inLow < 8u; ++inLow) {
			if ((inHigh | inLow) == 0u || hi < lo + inHigh + inLow + 1u) continue;
			const ChannelBlock candidate = EncodeWith(values, hi - inHigh, lo + inLow);
			if (candidate.error < best.error) best = candidate;
		}
	}
	if (innerHigh >= innerLow) {
		const uint innerLo = (uint)round(innerLow);
		const uint innerHi = (uint)round(innerHigh);
		// e0 <= e1 selects the palette with exact 0 and 255.
		const ChannelBlock withEnds = EncodeWith(values, innerLo, innerHi);
		if (withEnds.error < best.error) best = withEnds;
	}
	return best;
}

[numthreads(8, 8, 1)]
void BcChannelsCS(uint3 id : SV_DispatchThreadID)
{
	const uint blocksX = (g.width + 3u) / 4u;
	const uint blocksY = (g.height + 3u) / 4u;
	if (id.x >= blocksX || id.y >= blocksY) {
		return;
	}
	RWByteAddressBuffer source = ResourceDescriptorHeap[g.source];
	RWByteAddressBuffer destination = ResourceDescriptorHeap[g.destination];
	float red[16];
	float green[16];
	[unroll]
	for (uint y = 0u; y < 4u; ++y) {
		[unroll]
		for (uint x = 0u; x < 4u; ++x) {
			// A block past the image's edge (images under 4 texels) repeats its last row and column.
			const uint2 pixel = min(id.xy * 4u + uint2(x, y), uint2(g.width, g.height) - 1u.xx);
			const uint texel = source.Load(g.sourceOffset + (pixel.y * g.width + pixel.x) * 4u);
			red[y * 4u + x] = (float)(texel & 0xFFu);
			green[y * 4u + x] = (float)((texel >> 8u) & 0xFFu);
		}
	}
	const uint blockBytes = 8u * g.channels;
	const uint offset = g.destinationOffset + id.y * g.rowPitch + id.x * blockBytes;
	const ChannelBlock r = EncodeChannel(red);
	if (g.channels == 1u) {
		destination.Store2(offset, r.words);
		return;
	}
	const ChannelBlock gBlock = EncodeChannel(green);
	destination.Store4(offset, uint4(r.words, gBlock.words));
}
