// Which squares of a heightfield are seen from a group of viewpoints (VisibilityGpu.cpp): a launch per target and
// group. Each viewpoint within the radius casts rays at samples x samples points across the target's top, its edges
// included; the first that reaches its point unblocked by the ground makes the target visible. A target no viewpoint
// reaches is visible too, as nothing decides it. The payload is written by the miss and closest-hit shaders: NVIDIA
// loses it when the closest hit is skipped.
#include "Layout.hlsli"

cbuffer Push : register(b0)
{
	VisibilityConstants g;
};

struct Payload
{
	uint seen;
};

[shader("raygeneration")]
void VisibilityRayGen()
{
	const uint target = DispatchRaysIndex().x;
	const uint group = DispatchRaysIndex().y;
	ByteAddressBuffer input = ResourceDescriptorHeap[g.input];
	ByteAddressBuffer tops = ResourceDescriptorHeap[g.tops];
	RWByteAddressBuffer output = ResourceDescriptorHeap[g.output];
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[g.scene];

	const uint2 range = input.Load2(group * 8u);
	const float2 low = float2(target / g.rows, target % g.rows) * g.size;
	const float2 high = low + g.size;
	const float top = asfloat(tops.Load(target * 4u));
	// The samples reach the top's edges, where a ray from outside first meets it.
	const float step = g.samples > 1 ? g.size / (g.samples - 1) : 0.0f;
	const float start = g.samples > 1 ? 0.0f : 0.5f * g.size;
	bool reached = false;
	uint seen = 0;
	for (uint v = 0; v < range.y && seen == 0; ++v) {
		const float3 eye = asfloat(input.Load3(g.viewpointsOffset + (range.x + v) * 12u));
		if (g.radius > 0.0f && distance(eye.xy, clamp(eye.xy, low, high)) > g.radius) {
			continue;
		}
		reached = true;
		for (uint i = 0; i < g.samples * g.samples && seen == 0; ++i) {
			const float3 aim = float3(low + start + float2(i / g.samples, i % g.samples) * step, top);
			const float3 toAim = aim - eye;
			const float reach = length(toAim);
			RayDesc ray;
			ray.Origin = eye;
			ray.Direction = toAim / reach;
			ray.TMin = 0.0f;
			// Short of the point itself: the ground at a target's edge may meet its top.
			ray.TMax = reach * 0.999f;
			Payload payload;
			payload.seen = 0;
			TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, payload);
			seen = payload.seen;
		}
	}
	output.Store((group * g.targets + target) * 4u, reached ? seen : 1u);
}

[shader("miss")]
void VisibilityMiss(inout Payload a_payload)
{
	a_payload.seen = 1;
}

[shader("closesthit")]
void VisibilityHit(inout Payload a_payload, in BuiltInTriangleIntersectionAttributes a_attributes)
{
	a_payload.seen = 0;
}