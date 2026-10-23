// The buffers and push constants tesworld's GPU kernels share with the C++ that fills them (src/gpu/Layout.h compiles
// this file too, so the two can't drift). Plain 32-bit fields only; every offset is in bytes.
#ifndef TESWORLD_GPU_LAYOUT_HLSLI
#define TESWORLD_GPU_LAYOUT_HLSLI

static const uint kNone = 0xFFFFFFFFu;

// BakeConstants.flags
static const uint kBakeDiffuse = 1u;
static const uint kBakeNormal = 2u;
static const uint kBakeVertexColors = 4u;

// MaterialHeader.flags: TerrainMaterialSettings.
static const uint kMaterialStochasticDiffuse = 1u;
static const uint kMaterialStochasticNormal = 2u;
static const uint kMaterialGaussian = 4u;
static const uint kMaterialDerivativeNormals = 8u;

// StochasticGpu.mode: StochasticMode.
static const uint kStochasticDecorrelatedColor = 0u;
static const uint kStochasticNormalXY = 1u;

// Push constants of TerrainBake.hlsl: one square of terrain, its diffuse and normal images (RGBA8, rows north first) at
// byte offsets of the output buffer.
struct BakeConstants
{
	uint world;
	uint material;
	uint output;
	uint flags;
	uint diffuseOffset;
	uint normalOffset;
	uint diffuseSize;
	uint normalSize;
	float originX;
	float originY;
	float extent;
	float gain;
	float bias;
	float fallbackR;
	float fallbackG;
	float fallbackB;
};

// The world buffer: a worldspace's vertex lattice from (gx0, gy0), width x height points: heights (float) and LAND
// vertex colours (RGBA8, 255 alpha where the LAND has none: white). Points outside take the default height and white.
struct WorldHeader
{
	int gx0;
	int gy0;
	uint width;
	uint height;
	uint heightsOffset;
	uint colorsOffset;
	float defaultHeight;
	uint pad;
};

// A material buffer: a TerrainMaterial flattened. Regions sit in a dense grid from (rx0, ry0) of region index + 1 (0:
// none); the layer refs and weight blocks are TerrainWorldBuildResult's as they are.
struct MaterialHeader
{
	int rx0;
	int ry0;
	uint gridWidth;
	uint gridHeight;
	uint gridOffset;
	uint regionsOffset;
	uint layerRefsOffset;
	uint layerRefCount;
	uint weightsOffset;
	uint weightCount;
	uint layersOffset;
	uint layerCount;
	uint texturesOffset;
	uint levelsOffset;
	uint stochasticOffset;
	uint flags;
	float blendCurve;
	uint pad0;
	uint pad1;
	uint pad2;
};

struct RegionGpu
{
	uint layerRefStart;
	uint layerRefCount;
	uint weightBlockStart;
	uint weightSampleSide;
};

// Texture and stochastic references are indices into the material's tables, or kNone.
struct LayerGpu
{
	uint diffuse;
	uint normal;
	uint diffuseStochastic;
	uint normalStochastic;
	float uvScale;
	float stochasticScale;
	uint pad0;
	uint pad1;
	float fallbackR;
	float fallbackG;
	float fallbackB;
	float fallbackA;
};

// A mip chain: levelCount levels from firstLevel in the material's level table.
struct TextureGpu
{
	uint firstLevel;
	uint levelCount;
	uint width;
	uint height;
};

// One image: RGBA8 texels at a byte offset of a texture pool buffer (a descriptor).
struct LevelGpu
{
	uint pool;
	uint offset;
	uint width;
	uint height;
};

struct StochasticGpu
{
	uint gaussian;
	uint lut;
	uint lutHeight;
	uint mode;
	float originX;
	float originY;
	float originZ;
	float v0X;
	float v0Y;
	float v0Z;
	float v1X;
	float v1Y;
	float v1Z;
	float v2X;
	float v2Y;
	float v2Z;
};

// Push constants of Mips.hlsl: one level halved into the next, both RGBA8 in one buffer.
struct HalfSizeConstants
{
	uint buffer;
	uint sourceOffset;
	uint destinationOffset;
	uint sourceWidth;
	uint sourceHeight;
	uint destinationWidth;
	uint destinationHeight;
	uint pad;
};

// Push constants of Bc7Mode6.hlsl: one RGBA8 image to BC7 blocks, rowPitch bytes a row of blocks.
struct Bc7Constants
{
	uint source;
	uint sourceOffset;
	uint destination;
	uint destinationOffset;
	uint width;
	uint height;
	uint rowPitch;
	uint pad;
};

// Push constants of Visibility.hlsl: one ray launch per target and group. input holds each group's first viewpoint and
// count (two words), then from viewpointsOffset the viewpoints, three floats each; tops a float per target, in target
// order (x * rows + y); output a word per group and target, nonzero when visible. size is a target's width; radius 0
// traces every target.
struct VisibilityConstants
{
	uint scene;
	uint input;
	uint tops;
	uint output;
	uint rows;
	uint targets;
	uint samples;
	uint viewpointsOffset;
	float size;
	float radius;
	uint pad0;
	uint pad1;
};

// Push constants of Fill.hlsl (the device check): count words of value + index.
struct FillConstants
{
	uint buffer;
	uint count;
	uint value;
	uint pad;
};

// ViewDrawGpu.flags: what a shape has, as CpuRenderer reads it (Render.cpp).
static const uint kViewHasUv = 1u;
static const uint kViewHasNormals = 2u;
static const uint kViewHasTangents = 4u;
static const uint kViewVertexColors = 8u;
static const uint kViewVertexAlpha = 16u;
static const uint kViewCutout = 32u;
static const uint kViewModelSpace = 64u;

// A view frame's buffer: this header, then its vertices (kViewVertexBytes each: position, normal, tangent, bitangent,
// UV, colour, as floats), indices (uint), draws, textures (TextureGpu), levels (LevelGpu) and texels (RGBA8).
struct ViewFrameHeader
{
	uint vertexOffset;
	uint indexOffset;
	uint drawOffset;
	uint textureOffset;
	uint levelOffset;
	// The targets' size in samples, a power of two.
	uint targetSize;
	uint pad0;
	uint pad1;
};

static const uint kViewVertexBytes = 72u;

// One shape of one view, drawn into the view's rectangle of the frame's targets: its triangles from firstIndex, its
// vertices from vertexBase, the view's camera (Render.h's Camera) and depth range, its size in samples, and the shape's
// shader as CpuRenderer reads it. Textures are indices into the frame's table, kNone for none.
struct ViewDrawGpu
{
	uint firstIndex;
	uint triangleCount;
	uint vertexBase;
	uint flags;
	float centerX;
	float centerY;
	float centerZ;
	float width;
	float rightX;
	float rightY;
	float rightZ;
	float height;
	float upX;
	float upY;
	float upZ;
	float depthNear;
	float forwardX;
	float forwardY;
	float forwardZ;
	float depthFar;
	uint diffuse;
	uint normalMap;
	float samplesWide;
	float samplesHigh;
	float alpha;
	float threshold;
	float uvScaleX;
	float uvScaleY;
	float uvOffsetX;
	float uvOffsetY;
	float direct;
	float ambient;
	float lightX;
	float lightY;
	float lightZ;
	float pad;
};

// Push constants of View.hlsl: the frame's buffer (a descriptor), the draw, and the top left of its view's rectangle of
// the targets, in samples.
struct ViewDrawConstants
{
	uint frame;
	uint draw;
	uint x;
	uint y;
};

// Push constants of ViewResolve.hlsl: one view's samples (supersample x supersample a pixel, from x, y of the colour and
// normal targets) to its RGBA8 image and normal map at byte offsets of the output buffer, width x height pixels.
struct ViewResolveConstants
{
	uint color;
	uint normal;
	uint output;
	uint supersample;
	uint x;
	uint y;
	uint width;
	uint height;
	uint colorOffset;
	uint normalOffset;
	uint pad0;
	uint pad1;
	float rightX;
	float rightY;
	float rightZ;
	float pad2;
	float upX;
	float upY;
	float upZ;
	float pad3;
	float forwardX;
	float forwardY;
	float forwardZ;
	float pad4;
};

#endif
