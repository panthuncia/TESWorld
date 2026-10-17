#pragma once

#include "tesworld/Math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Meshes in a form that doesn't depend on the NIF library: a model read as world-ready triangle lists, and the scene
// graph to write a Skyrim SE NIF from. Backed by nifly (GPL-3.0); a binary that links tesworld::nif is GPL.
namespace tesworld::nif
{
	enum class ShaderKind
	{
		None,
		Lighting,
		Effect,
		Other
	};

	// Texture slots of a BSShaderTextureSet in Skyrim: diffuse, normal, glow/skin, height/parallax, environment,
	// environment mask, multilayer, backlight/specular, unused.
	inline constexpr std::size_t kTextureSlots = 9;

	struct Shader
	{
		ShaderKind kind{ ShaderKind::None };
		std::uint32_t shaderType{ 0 };
		std::uint32_t flags1{ 0 };
		std::uint32_t flags2{ 0 };
		std::array<std::string, kTextureSlots> textures{};
		// The effect shader's source texture, when kind is Effect.
		std::string effectTexture;
		Vec2 uvOffset{};
		Vec2 uvScale{ 1.0f, 1.0f };
		std::uint32_t textureClampMode{ 3 };
		float alpha{ 1.0f };
		float refractionStrength{ 0.0f };
		float glossiness{ 1.0f };
		Vec3 specularColor{ 1.0f, 1.0f, 1.0f };
		float specularStrength{ 1.0f };
		float softLighting{ 0.3f };
		float rimLightPower{ 2.0f };
		float environmentMapScale{ 1.0f };
		float parallaxInnerLayerThickness{ 0.0f };
		float parallaxRefractionScale{ 1.0f };
		Vec2 parallaxInnerLayerTextureScale{ 1.0f, 1.0f };
		float parallaxEnvironmentMapStrength{ 1.0f };
		Vec3 skinTintColor{ 1.0f, 1.0f, 1.0f };
		Vec3 hairTintColor{ 1.0f, 1.0f, 1.0f };
		float lightingMaxPasses{ 1.0f };
		float lightingScale{ 1.0f };
		Vec4 sparkleParameters{};
		float eyeCubemapScale{ 1.0f };
		Vec3 eyeLeftReflectionCenter{};
		Vec3 eyeRightReflectionCenter{};
		Vec3 emissiveColor{};
		float emissiveMultiple{ 1.0f };
		// NiAlphaProperty: blending and testing as the game reads its flags.
		bool alphaBlend{ false };
		bool alphaTest{ false };
		std::uint8_t alphaThreshold{ 128 };
		std::uint16_t alphaFlags{ 0 };

		[[nodiscard]] bool DoubleSided() const { return (flags2 & (1u << 4)) != 0; }
		[[nodiscard]] bool VertexColors() const { return (flags2 & (1u << 5)) != 0; }
		[[nodiscard]] bool VertexAlpha() const { return (flags1 & (1u << 3)) != 0; }
		[[nodiscard]] bool ModelSpaceNormals() const { return (flags1 & (1u << 12)) != 0; }
		[[nodiscard]] bool TreeAnim() const { return (flags2 & (1u << 29)) != 0; }
	};

	// A shape's geometry in model space (every node transform above it applied).
	struct Shape
	{
		std::string name;
		std::string blockType;
		std::vector<Vec3> positions;
		std::vector<Vec3> normals;
		std::vector<Vec3> tangents;
		std::vector<Vec3> bitangents;
		std::vector<Vec2> uvs;
		std::vector<Vec4> colors;
		std::vector<std::uint32_t> indices;
		Shader shader;
		bool skinned{ false };
	};

	struct Model
	{
		std::vector<Shape> shapes;
		// Shapes left out: hidden, collision, editor markers, particles, or geometry nifly can't read.
		std::uint32_t skippedShapes{ 0 };
	};

	[[nodiscard]] std::optional<Model> ReadModel(std::span<const std::uint8_t> a_bytes, std::string* a_error = nullptr);

	// Raw-file observations, including blocks not represented by the static triangle reader. No export or mutation.
	struct InspectedBlock
	{
		std::uint32_t id{ 0 };
		std::string type;
		bool opaque{ false };
		bool controller{ false };
		bool node{ false }, objectNet{ false };
		std::string name;
		std::optional<std::uint32_t> controllerRef, nextController, controllerTarget;
		std::vector<std::uint32_t> children;
		std::vector<std::uint32_t> pointers;
		std::vector<std::string> textures;
		bool shape{ false }, sceneObject{ false }, shaderProperty{ false };
		std::optional<std::uint32_t> shaderRef, textureSetRef;
		std::vector<std::uint32_t> sceneChildren;
		std::vector<std::string> textureSlots;
	};
	struct ModelStructure
	{
		std::uint32_t fileVersion{ 0 }, userVersion{ 0 }, streamVersion{ 0 };
		bool hasRoot{ false };
		std::vector<std::uint32_t> roots;
		std::vector<InspectedBlock> blocks;
	};
	[[nodiscard]] std::optional<ModelStructure> InspectStructure(std::span<const std::uint8_t> a_bytes, std::string* a_error = nullptr);

	struct TextureSetEdit
	{
		std::uint32_t block{}, slot{};
		std::string expected, replacement;
	};
	// Skyrim SE texture-set payload edits. All other blocks, header fields and footer bytes remain identical.
	// Expected strings guard stale edits; rejects noncanonical headers, malformed blocks and unsafe replacement paths.
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> RewriteTextureSets(std::span<const std::uint8_t> a_bytes,
		std::span<const TextureSetEdit> a_edits, std::string* a_error = nullptr);

	// ---- Writing ----

	// A BSSubIndexTriShape segment: a run of the index buffer (start counts indices, three per triangle).
	struct Segment
	{
		std::uint32_t startIndex{ 0 };
		std::uint32_t triangleCount{ 0 };
	};

	struct OutShape
	{
		std::string name;
		bool subIndex{ false };
		std::vector<Segment> segments;
		Transform transform;
		std::vector<Vec3> positions;
		std::vector<Vec3> normals;
		// Tangents and bitangents are computed from normals and UVs when left empty.
		std::vector<Vec3> tangents;
		std::vector<Vec3> bitangents;
		std::vector<Vec2> uvs;
		std::vector<Vec4> colors;
		// Indices into positions; a shape holds at most 65535 vertices.
		std::vector<std::uint16_t> indices;
		// Lighting shader unless kind is None (the shape then has no shader, like LOD water).
		Shader shader{ .kind = ShaderKind::Lighting };
		bool largeReference{ false };
	};

	enum class NodeType
	{
		NiNode,
		BSFadeNode,
		BSMultiBoundNode
	};

	struct AxisAlignedBox
	{
		Vec3 center{};
		Vec3 halfExtent{};
	};

	struct OutNode
	{
		NodeType type{ NodeType::NiNode };
		std::string name;
		Transform transform;
		std::uint32_t flags{ 14 };
		std::optional<AxisAlignedBox> multiBound;
		std::uint32_t cullingMode{ 0 };
		std::vector<OutNode> children;
		std::vector<OutShape> shapes;
	};

	// A Skyrim SE NIF (20.2.0.7, user 12, stream 100). a_output is replaced and keeps its capacity, so a caller writing
	// many files can reuse one vector; it is left as it was when writing fails.
	[[nodiscard]] bool WriteNif(const OutNode& a_root, std::vector<std::uint8_t>& a_output, std::string* a_error = nullptr);
}