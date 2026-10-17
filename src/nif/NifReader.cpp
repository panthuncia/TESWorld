#include "tesworld/Nif.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <spanstream>
#include <stdexcept>
#include <string_view>

#include <NifFile.hpp>
#include <Factory.hpp>

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
					shader.refractionStrength = lighting->refractionStrength;
					shader.glossiness = lighting->glossiness;
					shader.specularColor = { lighting->specularColor.x, lighting->specularColor.y, lighting->specularColor.z };
					shader.specularStrength = lighting->specularStrength;
					shader.softLighting = lighting->softlighting;
					shader.rimLightPower = lighting->rimlightPower;
					shader.environmentMapScale = lighting->environmentMapScale;
					shader.parallaxInnerLayerThickness = lighting->parallaxInnerLayerThickness;
					shader.parallaxRefractionScale = lighting->parallaxRefractionScale;
					shader.parallaxInnerLayerTextureScale = { lighting->parallaxInnerLayerTextureScale.u, lighting->parallaxInnerLayerTextureScale.v };
					shader.parallaxEnvironmentMapStrength = lighting->parallaxEnvmapStrength;
					shader.skinTintColor = { lighting->skinTintColor.x, lighting->skinTintColor.y, lighting->skinTintColor.z };
					shader.hairTintColor = { lighting->hairTintColor.x, lighting->hairTintColor.y, lighting->hairTintColor.z };
					shader.lightingMaxPasses = lighting->maxPasses;
					shader.lightingScale = lighting->scale;
					shader.sparkleParameters = { lighting->sparkleParameters.r, lighting->sparkleParameters.g, lighting->sparkleParameters.b, lighting->sparkleParameters.a };
					shader.eyeCubemapScale = lighting->eyeCubemapScale;
					shader.eyeLeftReflectionCenter = { lighting->eyeLeftReflectionCenter.x, lighting->eyeLeftReflectionCenter.y, lighting->eyeLeftReflectionCenter.z };
					shader.eyeRightReflectionCenter = { lighting->eyeRightReflectionCenter.x, lighting->eyeRightReflectionCenter.y, lighting->eyeRightReflectionCenter.z };
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

	std::optional<ModelStructure> InspectStructure(std::span<const std::uint8_t> a_bytes, std::string* a_error)
	{
		try {
			std::ispanstream stream(std::span<const char>(reinterpret_cast<const char*>(a_bytes.data()), a_bytes.size()));
			nifly::NiHeader header;
			nifly::NiIStream input(&stream, &header);
			header.Get(input);
			if (!header.IsValid() || header.GetVersion().File() < nifly::V20_2_0_5 || header.HasInlineBlockTypes())
				throw std::runtime_error("model inspection requires a modern NIF with block sizes");
			if (header.GetNumBlocks() > 100000) throw std::runtime_error("model inspection exceeds 100000 blocks");
			std::vector<std::unique_ptr<nifly::NiObject>> blocks;
			blocks.reserve(header.GetNumBlocks());
			nifly::NiFactoryRegister factories;
			// NifFile::Load prepares geometry and trims texture paths. Read blocks directly so inspection never conceals
			// invalid references or changes the texture strings it is meant to validate.
			for (std::uint32_t id = 0; id < header.GetNumBlocks(); ++id) {
				const auto before = stream.tellg();
				const auto size = header.GetBlockSize(id);
				if (before < 0 || static_cast<std::size_t>(before) > a_bytes.size() || size > a_bytes.size() - static_cast<std::size_t>(before))
					throw std::runtime_error("NIF block extends beyond its input");
				if (auto* factory = factories.GetFactoryByName(header.GetBlockTypeStringById(id))) blocks.push_back(factory->Load(input));
				else blocks.push_back(std::make_unique<nifly::NiUnknown>(input, size));
				if (!stream || stream.tellg() - before != static_cast<std::streamoff>(size)) throw std::runtime_error("NIF block size does not match its parsed contents");
			}
			header.GetFooter(input);
			if (!stream) throw std::runtime_error("NIF footer is incomplete");
			header.SetBlockReference(&blocks);
			header.FillStringRefs();
			const auto& version = header.GetVersion();
			ModelStructure result{ .fileVersion = static_cast<std::uint32_t>(version.File()), .userVersion = version.User(),
				.streamVersion = version.Stream(), .roots = header.GetRootBlockIds() };
			for (const auto root : result.roots) if (header.GetBlock<nifly::NiNode>(root)) result.hasRoot = true;
			result.blocks.reserve(header.GetNumBlocks());
			for (std::uint32_t id = 0; id < header.GetNumBlocks(); ++id) {
				auto* block = header.GetBlock<nifly::NiObject>(id);
				if (!block) throw std::runtime_error("missing NIF block");
				InspectedBlock observed{ .id = id, .type = header.GetBlockTypeStringById(id),
					.opaque = dynamic_cast<nifly::NiUnknown*>(block) != nullptr,
					.controller = dynamic_cast<nifly::NiTimeController*>(block) != nullptr };
				observed.node = dynamic_cast<nifly::NiNode*>(block) != nullptr;
				observed.sceneObject = dynamic_cast<nifly::NiAVObject*>(block) != nullptr;
				if (auto* node = dynamic_cast<nifly::NiNode*>(block))
					for (const auto& child : node->childRefs) observed.sceneChildren.push_back(child.index);
				if (const auto* shape = dynamic_cast<nifly::NiShape*>(block)) {
					observed.shape = true;
					if (const auto* shader = shape->ShaderPropertyRef(); shader && shader->index != nifly::NIF_NPOS) observed.shaderRef = shader->index;
				}
				if (const auto* shader = dynamic_cast<nifly::NiShader*>(block)) {
					observed.shaderProperty = true;
					if (const auto* texture = shader->TextureSetRef(); texture && texture->index != nifly::NIF_NPOS) observed.textureSetRef = texture->index;
				}
				if (const auto* object = dynamic_cast<nifly::NiObjectNET*>(block)) {
					observed.objectNet = true;
					observed.name = object->name.get();
					if (object->controllerRef.index != nifly::NIF_NPOS) observed.controllerRef = object->controllerRef.index;
				}
				if (const auto* controller = dynamic_cast<nifly::NiTimeController*>(block)) {
					if (controller->nextControllerRef.index != nifly::NIF_NPOS) observed.nextController = controller->nextControllerRef.index;
					if (controller->targetRef.index != nifly::NIF_NPOS) observed.controllerTarget = controller->targetRef.index;
				}
				std::set<nifly::NiRef*> children;
				std::set<nifly::NiPtr*> pointers;
				block->GetChildRefs(children);
				block->GetPtrs(pointers);
				for (const auto* ref : children) if (ref->index != nifly::NIF_NPOS) observed.children.push_back(ref->index);
				for (const auto* ref : pointers) if (ref->index != nifly::NIF_NPOS) observed.pointers.push_back(ref->index);
				std::ranges::sort(observed.children);
				std::ranges::sort(observed.pointers);
				if (auto* textures = dynamic_cast<nifly::BSShaderTextureSet*>(block))
					for (const auto& texture : textures->textures) {
						observed.textureSlots.push_back(texture.get());
						if (!texture.get().empty()) observed.textures.push_back(texture.get());
					}
				if (auto* effect = dynamic_cast<nifly::BSEffectShaderProperty*>(block)) {
					for (const auto* texture : { &effect->sourceTexture, &effect->greyscaleTexture, &effect->envMapTexture, &effect->normalTexture,
						&effect->envMaskTexture, &effect->reflectanceTexture, &effect->lightingTexture, &effect->emitGradientTexture })
						if (!texture->get().empty()) observed.textures.push_back(texture->get());
				}
				result.blocks.push_back(std::move(observed));
			}
			return result;
		} catch (const std::exception& ex) {
			if (a_error) *a_error = ex.what();
			return std::nullopt;
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