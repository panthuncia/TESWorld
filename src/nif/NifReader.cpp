#include "tesworld/Nif.h"

#include <algorithm>
#include <cctype>
#include <spanstream>
#include <string_view>

#include <NifFile.hpp>

namespace tesworld::nif
{
	namespace
	{
		Transform FromNifly(const nifly::MatTransform& a_transform)
		{
			Transform result;
			for (int i = 0; i < 3; ++i) {
				result.rotation[i] = { a_transform.rotation[i].x, a_transform.rotation[i].y, a_transform.rotation[i].z };
			}
			result.translation = { a_transform.translation.x, a_transform.translation.y, a_transform.translation.z };
			result.scale = a_transform.scale;
			return result;
		}

		bool IsEditorOnly(std::string_view a_name)
		{
			std::string lower(a_name);
			std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return lower.starts_with("editormarker") || lower.starts_with("markerx") || lower.starts_with("marker_") || lower == "marker";
		}

		Shader ReadShader(nifly::NifFile& a_nif, nifly::NiShape* a_shape)
		{
			Shader shader;
			if (auto* source = a_nif.GetShader(a_shape)) {
				if (auto* lighting = dynamic_cast<nifly::BSLightingShaderProperty*>(source)) {
					shader.kind = ShaderKind::Lighting;
					shader.flags1 = lighting->shaderFlags1;
					shader.flags2 = lighting->shaderFlags2;
					shader.shaderType = lighting->GetShaderType();
					shader.uvOffset = { lighting->uvOffset.u, lighting->uvOffset.v };
					shader.uvScale = { lighting->uvScale.u, lighting->uvScale.v };
					shader.textureClampMode = static_cast<std::uint32_t>(lighting->textureClampMode);
					shader.alpha = lighting->alpha;
					shader.glossiness = lighting->glossiness;
					shader.emissiveColor = { lighting->emissiveColor.x, lighting->emissiveColor.y, lighting->emissiveColor.z };
					shader.emissiveMultiple = lighting->emissiveMultiple;
				} else if (auto* effect = dynamic_cast<nifly::BSEffectShaderProperty*>(source)) {
					shader.kind = ShaderKind::Effect;
					shader.flags1 = effect->shaderFlags1;
					shader.flags2 = effect->shaderFlags2;
					shader.uvOffset = { effect->uvOffset.u, effect->uvOffset.v };
					shader.uvScale = { effect->uvScale.u, effect->uvScale.v };
					shader.textureClampMode = effect->textureClampMode;
					shader.effectTexture = effect->sourceTexture.get();
				} else {
					shader.kind = ShaderKind::Other;
				}
				for (std::uint32_t slot = 0; slot < kTextureSlots; ++slot) {
					std::string texture;
					a_nif.GetTextureSlot(a_shape, texture, slot);
					shader.textures[slot] = std::move(texture);
				}
			}
			if (auto* alpha = a_nif.GetAlphaProperty(a_shape)) {
				shader.alphaFlags = alpha->flags;
				shader.alphaBlend = (alpha->flags & 0x0001u) != 0;
				shader.alphaTest = (alpha->flags & 0x0200u) != 0;
				shader.alphaThreshold = alpha->threshold;
			}
			return shader;
		}

		void ReadShape(nifly::NifFile& a_nif, nifly::NiShape* a_shape, const Transform& a_toModel, Model& a_model)
		{
			std::vector<nifly::Vector3> positions;
			std::vector<nifly::Triangle> triangles;
			if (!a_nif.GetVertsForShape(a_shape, positions) || positions.empty() || !a_shape->GetTriangles(triangles) || triangles.empty()) {
				++a_model.skippedShapes;
				return;
			}

			Shape shape;
			shape.name = a_shape->name.get();
			shape.blockType = a_shape->GetBlockName();
			shape.skinned = a_shape->IsSkinned();
			shape.shader = ReadShader(a_nif, a_shape);

			// Skinned geometry is posed by its bones, not its nodes; the bind pose is the closest static form.
			const auto transform = shape.skinned ? Transform{} : a_toModel * FromNifly(a_shape->GetTransformToParent());
			shape.positions.reserve(positions.size());
			for (const auto& p : positions) {
				shape.positions.push_back(transform.Apply({ p.x, p.y, p.z }));
			}
			const auto rotate = [&](const nifly::Vector3& v) { return Normalize(transform.RotateVector({ v.x, v.y, v.z })); };
			if (const auto* normals = a_nif.GetNormalsForShape(a_shape); normals && normals->size() == positions.size()) {
				shape.normals.reserve(normals->size());
				for (const auto& n : *normals) shape.normals.push_back(rotate(n));
			}
			std::vector<nifly::Vector3> vectors;
			if (a_nif.GetTangentsForShape(a_shape, vectors) && vectors.size() == positions.size()) {
				for (const auto& t : vectors) shape.tangents.push_back(rotate(t));
			}
			vectors.clear();
			if (a_nif.GetBitangentsForShape(a_shape, vectors) && vectors.size() == positions.size()) {
				for (const auto& b : vectors) shape.bitangents.push_back(rotate(b));
			}
			std::vector<nifly::Vector2> uvs;
			if (a_nif.GetUvsForShape(a_shape, uvs) && uvs.size() == positions.size()) {
				shape.uvs.reserve(uvs.size());
				for (const auto& uv : uvs) shape.uvs.push_back({ uv.u, uv.v });
			}
			std::vector<nifly::Color4> colors;
			if (a_nif.GetColorsForShape(a_shape, colors) && colors.size() == positions.size()) {
				shape.colors.reserve(colors.size());
				for (const auto& c : colors) shape.colors.push_back({ c.r, c.g, c.b, c.a });
			}

			shape.indices.reserve(triangles.size() * 3);
			const auto count = static_cast<std::uint32_t>(positions.size());
			for (const auto& triangle : triangles) {
				if (triangle.p1 >= count || triangle.p2 >= count || triangle.p3 >= count ||
					triangle.p1 == triangle.p2 || triangle.p2 == triangle.p3 || triangle.p1 == triangle.p3) {
					continue;
				}
				shape.indices.insert(shape.indices.end(), { triangle.p1, triangle.p2, triangle.p3 });
			}
			if (shape.indices.empty()) {
				++a_model.skippedShapes;
				return;
			}
			a_model.shapes.push_back(std::move(shape));
		}

		void Walk(nifly::NifFile& a_nif, nifly::NiAVObject* a_object, const Transform& a_parentToModel, Model& a_model, int a_depth)
		{
			if (!a_object || a_depth > 64) {
				return;
			}
			// Hidden objects (flag bit 0) and editor markers never render.
			if ((a_object->flags & 0x1u) != 0 || IsEditorOnly(a_object->name.get())) {
				if (dynamic_cast<nifly::NiShape*>(a_object)) {
					++a_model.skippedShapes;
				}
				return;
			}
			if (auto* shape = dynamic_cast<nifly::NiShape*>(a_object)) {
				ReadShape(a_nif, shape, a_parentToModel, a_model);
				return;
			}
			if (auto* node = dynamic_cast<nifly::NiNode*>(a_object)) {
				const auto toModel = a_parentToModel * FromNifly(node->GetTransformToParent());
				// A switch node shows one child at a time; LOD takes the first.
				const bool firstOnly = std::string_view(node->GetBlockName()) == "NiSwitchNode";
				for (auto& child : node->childRefs) {
					Walk(a_nif, a_nif.GetHeader().GetBlock<nifly::NiAVObject>(child), toModel, a_model, a_depth + 1);
					if (firstOnly) {
						break;
					}
				}
			}
		}
	}

	std::optional<Model> ReadModel(std::span<const std::uint8_t> a_bytes, std::string* a_error)
	{
		try {
			std::ispanstream stream(std::span<const char>(reinterpret_cast<const char*>(a_bytes.data()), a_bytes.size()));
			nifly::NifFile nif;
			if (nif.Load(stream) != 0 || !nif.IsValid()) {
				if (a_error) *a_error = "not a NIF nifly can read";
				return std::nullopt;
			}
			Model model;
			// A placed reference replaces its model root's transform with its own, so the root's is never applied.
			if (auto* root = nif.GetRootNode()) {
				for (auto& child : root->childRefs) {
					Walk(nif, nif.GetHeader().GetBlock<nifly::NiAVObject>(child), Transform{}, model, 1);
				}
			}
			return model;
		} catch (const std::exception& ex) {
			if (a_error) *a_error = ex.what();
			return std::nullopt;
		}
	}
}
