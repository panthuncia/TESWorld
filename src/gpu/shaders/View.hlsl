// A view's shapes drawn as CpuRenderer draws them (Render.cpp): orthographic, both faces of every triangle facing the
// camera, alpha-tested, lit by vertex normals or model-space and tangent-space normal maps, colour and normal written
// per sample for ViewResolve.hlsl to box-filter. Vertices are pulled from the frame's buffer; a triangle's texture level
// is the CPU's, from its UV area against its area in samples, and what's drawn at a sample is too.
//
// Views share a frame's targets, each in its own rectangle, drawn under one viewport over the whole target: a vertex is
// snapped to the rasterizer's grid (1/256 sample) in its view, then moved by its rectangle's whole-sample corner, so it
// lands at exactly the same place within the rectangle wherever that is, and a view comes out the same however it's
// batched. Samples outside the rectangle are discarded.
#include "Sampling.hlsli"

cbuffer Push : register(b0)
{
	ViewDrawConstants g;
};

struct ViewVertex
{
	float3 position;
	float3 normal;
	float3 tangent;
	float3 bitangent;
	float2 uv;
	float4 color;
};

ViewFrameHeader LoadHeader(ByteAddressBuffer a_frame)
{
	return a_frame.Load<ViewFrameHeader>(0);
}

ViewDrawGpu LoadDraw(ByteAddressBuffer a_frame, ViewFrameHeader a_header)
{
	return a_frame.Load<ViewDrawGpu>(a_header.drawOffset + g.draw * 144u);
}

uint LoadIndex(ByteAddressBuffer a_frame, ViewFrameHeader a_header, ViewDrawGpu a_draw, uint a_corner)
{
	return a_draw.vertexBase + a_frame.Load(a_header.indexOffset + (a_draw.firstIndex + a_corner) * 4u);
}

ViewVertex LoadVertex(ByteAddressBuffer a_frame, ViewFrameHeader a_header, uint a_vertex)
{
	const uint at = a_header.vertexOffset + a_vertex * kViewVertexBytes;
	ViewVertex vertex;
	vertex.position = asfloat(a_frame.Load3(at));
	vertex.normal = asfloat(a_frame.Load3(at + 12u));
	vertex.tangent = asfloat(a_frame.Load3(at + 24u));
	vertex.bitangent = asfloat(a_frame.Load3(at + 36u));
	vertex.uv = asfloat(a_frame.Load2(at + 48u));
	vertex.color = asfloat(a_frame.Load4(at + 56u));
	return vertex;
}

float3 Center(ViewDrawGpu a_draw) { return float3(a_draw.centerX, a_draw.centerY, a_draw.centerZ); }
float3 Right(ViewDrawGpu a_draw) { return float3(a_draw.rightX, a_draw.rightY, a_draw.rightZ); }
float3 Up(ViewDrawGpu a_draw) { return float3(a_draw.upX, a_draw.upY, a_draw.upZ); }
float3 Forward(ViewDrawGpu a_draw) { return float3(a_draw.forwardX, a_draw.forwardY, a_draw.forwardZ); }

// Math.h's Normalize: a_fallback when a_value is (nearly) nothing.
float3 NormalizeOr(float3 a_value, float3 a_fallback)
{
	const float length = sqrt(dot(a_value, a_value));
	return length > 1e-12f ? a_value * (1.0f / length) : a_fallback;
}

// Where a point lands in the view, in samples from its top left: CpuRenderer's sx, sy.
float2 Project(ViewDrawGpu a_draw, float3 a_position)
{
	precise float3 d = a_position - Center(a_draw);
	precise float sx = (dot(d, Right(a_draw)) / a_draw.width + 0.5f) * a_draw.samplesWide;
	precise float sy = (0.5f - dot(d, Up(a_draw)) / a_draw.height) * a_draw.samplesHigh;
	return float2(sx, sy);
}

TextureGpu LoadViewTexture(ByteAddressBuffer a_frame, ViewFrameHeader a_header, uint a_texture)
{
	return a_frame.Load<TextureGpu>(a_header.textureOffset + a_texture * 16u);
}

// Texture.cpp's SampleLevel, wrapping.
float4 SampleView(ByteAddressBuffer a_frame, ViewFrameHeader a_header, TextureGpu a_texture, float2 a_uv, float a_lod)
{
	if (a_texture.levelCount == 0u) {
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	const float lod = clamp(a_lod, 0.0f, float(a_texture.levelCount - 1u));
	const uint low = uint(floor(lod));
	const uint high = min(low + 1u, a_texture.levelCount - 1u);
	precise float t = lod - float(low);
	const float4 a = SampleBilinear(a_frame.Load<LevelGpu>(a_header.levelOffset + (a_texture.firstLevel + low) * 16u), a_uv, true);
	if (t <= 0.0f || high == low) {
		return a;
	}
	const float4 b = SampleBilinear(a_frame.Load<LevelGpu>(a_header.levelOffset + (a_texture.firstLevel + high) * 16u), a_uv, true);
	precise float4 result = a + (b - a) * t;
	return result;
}

// The hardware only decides which samples a triangle covers: what's drawn there is worked out from the triangle's
// corners at the sample's place in the view, as the CPU works it out.
struct Interpolants
{
	float4 position : SV_Position;
};

Interpolants ViewVS(uint a_vertex : SV_VertexID)
{
	ByteAddressBuffer frame = ResourceDescriptorHeap[g.frame];
	const ViewFrameHeader header = LoadHeader(frame);
	const ViewDrawGpu draw = LoadDraw(frame, header);
	const ViewVertex vertex = LoadVertex(frame, header, LoadIndex(frame, header, draw, a_vertex));
	const float2 at = round(Project(draw, vertex.position) * 256.0f) / 256.0f + float2(g.x, g.y);
	const float half = float(header.targetSize / 2u);
	Interpolants output;
	output.position = float4(at.x / half - 1.0f, 1.0f - at.y / half, 0.5f, 1.0f);
	return output;
}

struct Targets
{
	float4 color : SV_Target0;
	float4 normal : SV_Target1;
	float depth : SV_Depth;
};

Targets ViewPS(Interpolants a_in, uint a_primitive : SV_PrimitiveID)
{
	ByteAddressBuffer frame = ResourceDescriptorHeap[g.frame];
	const ViewFrameHeader header = LoadHeader(frame);
	const ViewDrawGpu draw = LoadDraw(frame, header);
	// The sample's centre in the view, as the CPU's (x + 0.5, y + 0.5): exact, wherever the view's rectangle is.
	const float px = a_in.position.x - float(g.x), py = a_in.position.y - float(g.y);
	if (px < 0.0f || py < 0.0f || px >= draw.samplesWide || py >= draw.samplesHigh) {
		discard;
	}
	const float3 toCamera = -Forward(draw);

	// The triangle as the CPU sees it: its corners, its area in samples, its facing, and the sample's weights.
	const ViewVertex a = LoadVertex(frame, header, LoadIndex(frame, header, draw, a_primitive * 3u));
	const ViewVertex b = LoadVertex(frame, header, LoadIndex(frame, header, draw, a_primitive * 3u + 1u));
	const ViewVertex c = LoadVertex(frame, header, LoadIndex(frame, header, draw, a_primitive * 3u + 2u));
	const float2 sa = Project(draw, a.position), sb = Project(draw, b.position), sc = Project(draw, c.position);
	precise float area = (sb.x - sa.x) * (sc.y - sa.y) - (sc.x - sa.x) * (sb.y - sa.y);
	precise float w0 = ((sb.x - px) * (sc.y - py) - (sc.x - px) * (sb.y - py)) / area;
	precise float w1 = ((sc.x - px) * (sa.y - py) - (sa.x - px) * (sc.y - py)) / area;
	precise float w2 = 1.0f - w0 - w1;
	const float3 faceNormal = NormalizeOr(cross(b.position - a.position, c.position - a.position), toCamera);
	const float3 center = Center(draw), forward = Forward(draw);
	precise float z = dot(a.position - center, forward) * w0 + dot(b.position - center, forward) * w1 + dot(c.position - center, forward) * w2;

	const bool hasUv = (draw.flags & kViewHasUv) != 0u;
	const bool cutout = (draw.flags & kViewCutout) != 0u;
	TextureGpu diffuse = (TextureGpu)0;
	if (draw.diffuse != kNone) {
		diffuse = LoadViewTexture(frame, header, draw.diffuse);
	}
	// The texture level matching the triangle's texels per sample.
	float lod = 0.0f;
	if (draw.diffuse != kNone && hasUv) {
		precise float uvArea = abs((b.uv.x - a.uv.x) * (c.uv.y - a.uv.y) - (c.uv.x - a.uv.x) * (b.uv.y - a.uv.y)) * float(diffuse.width) * float(diffuse.height);
		lod = max(0.0f, 0.5f * log2(max(uvArea / abs(area), 1e-12f)));
	}

	float4 albedo = float4(0.6f, 0.6f, 0.6f, draw.alpha);
	float2 uv = float2(0.0f, 0.0f);
	if (hasUv) {
		precise float u = (a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2) * draw.uvScaleX + draw.uvOffsetX;
		precise float v = (a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2) * draw.uvScaleY + draw.uvOffsetY;
		uv = float2(u, v);
		if (draw.diffuse != kNone) {
			const float4 texel = SampleView(frame, header, diffuse, uv, lod);
			// A cutout's alpha is tested at the full size: smaller levels average it away, and leaves thin out.
			const float alpha = cutout ? SampleView(frame, header, diffuse, uv, 0.0f).w : texel.w;
			albedo = float4(texel.xyz, alpha * draw.alpha);
		}
	}
	if ((draw.flags & kViewVertexColors) != 0u) {
		precise float4 color = a.color * w0 + b.color * w1 + c.color * w2;
		albedo *= float4(color.xyz, (draw.flags & kViewVertexAlpha) != 0u ? color.w : 1.0f);
	}
	if (cutout && albedo.w < draw.threshold) {
		discard;
	}

	// The surface's normal, turned to face the camera: both faces are drawn.
	float3 n = faceNormal;
	if ((draw.flags & kViewHasNormals) != 0u) {
		precise float3 normal = a.normal * w0 + b.normal * w1 + c.normal * w2;
		n = NormalizeOr(normal, faceNormal);
		if (draw.normalMap != kNone && hasUv) {
			const float4 texel = SampleView(frame, header, LoadViewTexture(frame, header, draw.normalMap), uv, lod);
			if ((draw.flags & kViewModelSpace) != 0u) {
				n = NormalizeOr(float3(texel.x * 2.0f - 1.0f, texel.z * 2.0f - 1.0f, texel.y * 2.0f - 1.0f), n);
			} else if ((draw.flags & kViewHasTangents) != 0u) {
				// Skyrim's tangent-space maps are green-up (OpenGL).
				precise float3 tangent = a.tangent * w0 + b.tangent * w1 + c.tangent * w2;
				precise float3 bitangent = a.bitangent * w0 + b.bitangent * w1 + c.bitangent * w2;
				const float3 t = texel.xyz * 2.0f - 1.0f;
				n = NormalizeOr(NormalizeOr(tangent, float3(0.0f, 0.0f, 1.0f)) * t.x + NormalizeOr(bitangent, float3(0.0f, 0.0f, 1.0f)) * t.y + n * t.z, n);
			}
		}
		n = NormalizeOr(n, faceNormal);
	}
	if (dot(n, toCamera) < 0.0f) {
		n = -n;
	}

	const float shade = draw.ambient + draw.direct * max(0.0f, dot(n, float3(draw.lightX, draw.lightY, draw.lightZ)));
	Targets output;
	output.color = float4(albedo.xyz * shade, 1.0f);
	output.normal = float4(n, 1.0f);
	// The CPU's depth, the nearer winning: what's drawn first wins a tie, as it does there.
	output.depth = saturate((z - draw.depthNear) / (draw.depthFar - draw.depthNear));
	return output;
}
