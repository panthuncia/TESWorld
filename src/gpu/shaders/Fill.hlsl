// The device check (Context::SelfTest): writes value + index into count words.
#include "Layout.hlsli"

cbuffer Push : register(b0)
{
	FillConstants g;
};

[numthreads(64, 1, 1)]
void FillCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g.count) {
		return;
	}
	RWByteAddressBuffer buffer = ResourceDescriptorHeap[g.buffer];
	buffer.Store(id.x * 4u, g.value + id.x);
}
