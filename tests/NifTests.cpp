// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Nif.h"

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
	assert(model->shapes[1].shader.kind == ShaderKind::None);
	assert(model->shapes[1].colors.empty());

	assert(!ReadModel(std::vector<std::uint8_t>(64, 0x41), &error));
	std::cout << "tesworld nif tests passed\n";
	return 0;
}
