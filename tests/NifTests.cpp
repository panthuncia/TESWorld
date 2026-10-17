// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Nif.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace tesworld;
using namespace tesworld::nif;

namespace
{
	bool Near(float a, float b, float tolerance = 1e-3f) { return std::abs(a - b) <= tolerance; }

	OutShape Quad(std::string a_name, float a_z)
	{
		OutShape shape;
		shape.name = std::move(a_name);
		shape.positions = { { 0, 0, a_z }, { 100, 0, a_z }, { 0, 100, a_z }, { 100, 100, a_z } };
		shape.normals = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
		shape.uvs = { { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 0 } };
		shape.colors = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 0.5f } };
		shape.indices = { 0, 1, 2, 2, 1, 3 };
		shape.shader.flags1 = 0x80000300u;
		shape.shader.flags2 = 0x25u;
		shape.shader.textures[0] = "textures\\lod\\rock.dds";
		shape.shader.textures[1] = "textures\\lod\\rock_n.dds";
		return shape;
	}
}

int main()
{
	// A block like an object LOD BTO: a node placed at the quad, a multibound node holding a segmented shape.
	OutNode root{ .type = NodeType::NiNode, .name = "obj" };
	OutNode bound{ .type = NodeType::BSMultiBoundNode, .name = "", .multiBound = AxisAlignedBox{ { 50, 50, 10 }, { 50, 50, 10 } }, .cullingMode = 1 };
	auto shape = Quad("objSnow", 10.0f);
	shape.subIndex = true;
	shape.segments = { { 0, 1 }, { 3, 1 } };
	shape.transform.translation = { 4096.0f * 3, 4096.0f * 2, 0.0f };
	shape.transform.scale = 4.0f;
	shape.largeReference = true;
	shape.shader.alphaTest = true;
	bound.shapes.push_back(std::move(shape));
	root.children.push_back(std::move(bound));

	OutNode plain{ .type = NodeType::BSFadeNode, .name = "water" };
	auto water = Quad("water", 0.0f);
	water.shader.kind = ShaderKind::None;
	water.colors.clear();
	plain.shapes.push_back(std::move(water));
	root.children.push_back(std::move(plain));

	std::vector<std::uint8_t> bytes;
	std::string error;
	assert(WriteNif(root, bytes, &error));
	assert(error.empty() && bytes.size() > 100);

	const auto model = ReadModel(bytes, &error);
	assert(model && model->shapes.size() == 2);
	const auto& objSnow = model->shapes[0];
	assert(objSnow.name == "objSnow");
	assert(objSnow.blockType == "BSSubIndexTriShape");
	assert(objSnow.indices.size() == 6);
	// The shape's transform is applied when read: scale 4 then the quad offset.
	assert(Near(objSnow.positions[1].x, 12288.0f + 400.0f) && Near(objSnow.positions[1].z, 40.0f));
	assert(objSnow.uvs.size() == 4 && Near(objSnow.uvs[1].x, 1.0f));
	assert(objSnow.colors.size() == 4 && Near(objSnow.colors[3].w, 0.5f, 0.01f));
	assert(objSnow.normals.size() == 4 && Near(objSnow.normals[0].z, 1.0f, 0.01f));
	assert(objSnow.shader.kind == ShaderKind::Lighting);
	assert(objSnow.shader.flags1 == 0x80000300u && objSnow.shader.flags2 == 0x25u);
	assert(objSnow.shader.textures[0] == "textures\\lod\\rock.dds");
	assert(objSnow.shader.alphaTest && objSnow.shader.alphaThreshold == 128);
	assert(Near(objSnow.shader.specularColor.x, 1.0f) && Near(objSnow.shader.specularStrength, 1.0f));
	assert(Near(objSnow.shader.parallaxInnerLayerTextureScale.x, 1.0f) && Near(objSnow.shader.parallaxRefractionScale, 1.0f));
	assert(model->shapes[1].shader.kind == ShaderKind::None);
	assert(model->shapes[1].colors.empty());

	const auto structure = InspectStructure(bytes, &error);
	assert(structure && structure->hasRoot && structure->fileVersion == 0x14020007u);
	assert(structure->userVersion == 12 && structure->streamVersion == 100);
	bool textureFound = false;
	for (std::size_t i = 0; i < structure->blocks.size(); ++i) {
		const auto& block = structure->blocks[i];
		assert(block.id == i && !block.opaque && !block.controller);
		for (const auto child : block.children) assert(child < structure->blocks.size());
		for (const auto& texture : block.textures) if (texture == "textures\\lod\\rock.dds") textureFound = true;
	}
	assert(textureFound);
	const auto textureBlock = std::ranges::find_if(structure->blocks, [](const auto& block) {
		return block.type == "BSShaderTextureSet" && std::ranges::find(block.textures, "textures\\lod\\rock.dds") != block.textures.end();
	});
	assert(textureBlock != structure->blocks.end());
	assert(textureBlock->textureSlots.size() == 9 && textureBlock->textureSlots[0] == "textures\\lod\\rock.dds");
	assert(textureBlock->textureSlots[8].empty());
	const auto rawShape = std::ranges::find_if(structure->blocks, [](const auto& block) { return block.shape && block.name == "objSnow"; });
	assert(rawShape != structure->blocks.end() && rawShape->sceneObject && rawShape->shaderRef);
	const auto& rawShader = structure->blocks[*rawShape->shaderRef];
	assert(rawShader.shaderProperty && rawShader.textureSetRef == textureBlock->id);
	const auto& rawRoot = structure->blocks[structure->roots[0]];
	assert(rawRoot.sceneChildren.size() == 2 && structure->blocks[rawRoot.sceneChildren[0]].type == "BSMultiBoundNode");
	const std::array<TextureSetEdit, 1> edits{ TextureSetEdit{textureBlock->id, 0, "textures\\lod\\rock.dds", "textures\\prepared\\changed.dds"} };
	const auto prepared = RewriteTextureSets(bytes, edits, &error);
	assert(prepared);
	const auto preparedModel = ReadModel(*prepared, &error);
	assert(preparedModel && preparedModel->shapes.size() == model->shapes.size());
	assert(preparedModel->shapes[0].shader.textures[0] == "textures\\prepared\\changed.dds");
	assert(Near(preparedModel->shapes[0].positions[1].x, objSnow.positions[1].x));
	assert(preparedModel->shapes[0].shader.alphaFlags == objSnow.shader.alphaFlags);
	assert(RewriteTextureSets(bytes, {}, &error) == bytes);
	auto stale = edits; stale[0].expected = "textures\\stale.dds";
	assert(!RewriteTextureSets(bytes, stale, &error));
	auto brokenFooter = bytes;
	for (std::size_t i = brokenFooter.size() - 8; i < brokenFooter.size() - 4; ++i) brokenFooter[i] = 0xff;
	assert(!RewriteTextureSets(brokenFooter, edits, &error));
	// Raw inspection must retain paths the ordinary nifly load would trim before a caller can validate them.
	const std::string unsafeTexture = "C:\\mods\\Data\\textures\\outside.dds";
	root.children[0].shapes[0].shader.textures[0] = unsafeTexture;
	std::vector<std::uint8_t> unsafeBytes;
	assert(WriteNif(root, unsafeBytes, &error));
	const auto raw = InspectStructure(unsafeBytes, &error);
	assert(raw);
	bool unsafeFound = false;
	for (const auto& block : raw->blocks)
		for (const auto& texture : block.textures) if (texture == unsafeTexture) unsafeFound = true;
	assert(unsafeFound);
	assert(!InspectStructure(std::vector<std::uint8_t>(64, 0x41), &error));
	assert(!ReadModel(std::vector<std::uint8_t>(64, 0x41), &error));
	// Independent write/reopen coverage for common and conditional SE lighting parameters.
	for (const std::uint32_t type : {0u, 1u, 5u, 6u, 7u, 11u, 14u, 16u}) {
		OutNode materialRoot; auto material = Quad("material", 0); auto& input = material.shader;
		input.shaderType = type; input.refractionStrength = 0.15f; input.glossiness = 0.04f;
		input.specularColor = {0.2f, 0.3f, 0.4f}; input.specularStrength = 0.7f; input.softLighting = 0.25f; input.rimLightPower = 0.75f;
		input.environmentMapScale = 0.6f; input.skinTintColor = {0.3f, 0.4f, 0.5f}; input.hairTintColor = {0.4f, 0.5f, 0.6f};
		input.lightingMaxPasses = 3; input.lightingScale = 0.8f;
		input.parallaxInnerLayerThickness = 0.2f; input.parallaxRefractionScale = 0.3f;
		input.parallaxInnerLayerTextureScale = {1.2f, 1.3f}; input.parallaxEnvironmentMapStrength = 0.4f;
		input.sparkleParameters = {0.1f, 0.2f, 0.3f, 0.4f}; input.eyeCubemapScale = 0.5f;
		input.eyeLeftReflectionCenter = {1, 2, 3}; input.eyeRightReflectionCenter = {-1, -2, -3};
		materialRoot.shapes.push_back(material);
		std::vector<std::uint8_t> written; assert(WriteNif(materialRoot, written, &error));
		const auto reopened = ReadModel(written, &error); assert(reopened && reopened->shapes.size() == 1);
		const auto& output = reopened->shapes[0].shader;
		assert(output.shaderType == type && Near(output.refractionStrength, 0.15f) && Near(output.glossiness, 0.04f));
		assert(Near(output.specularColor.x, 0.2f) && Near(output.specularColor.y, 0.3f) && Near(output.specularColor.z, 0.4f));
		assert(Near(output.specularStrength, 0.7f) && Near(output.softLighting, 0.25f) && Near(output.rimLightPower, 0.75f));
		if (type == 1) assert(Near(output.environmentMapScale, 0.6f));
		if (type == 5) assert(Near(output.skinTintColor.x, 0.3f));
		if (type == 6) assert(Near(output.hairTintColor.z, 0.6f));
		if (type == 7) assert(Near(output.lightingMaxPasses, 3) && Near(output.lightingScale, 0.8f));
		if (type == 11) assert(Near(output.parallaxInnerLayerThickness, 0.2f) && Near(output.parallaxRefractionScale, 0.3f)
			&& Near(output.parallaxInnerLayerTextureScale.x, 1.2f) && Near(output.parallaxInnerLayerTextureScale.y, 1.3f) && Near(output.parallaxEnvironmentMapStrength, 0.4f));
		if (type == 14) assert(Near(output.sparkleParameters.w, 0.4f));
		if (type == 16) assert(Near(output.eyeCubemapScale, 0.5f) && Near(output.eyeLeftReflectionCenter.x, 1) && Near(output.eyeRightReflectionCenter.z, -3));
	}

	std::cout << "tesworld nif tests passed\n";
	return 0;
}