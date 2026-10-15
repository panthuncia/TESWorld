#include "NifWriterReference.h"

#include "tesworld/Profiling.h"

#include <algorithm>
#include <limits>
#include <sstream>

#include <NifFile.hpp>

// The writer WriteNif replaced, kept as it was: nifly builds the scene graph and saves it. The tests compare the two.
namespace tesworld::nif::detail
{
	namespace
	{
		nifly::MatTransform ToNifly(const Transform& a_transform)
		{
			nifly::MatTransform result;
			for (int i = 0; i < 3; ++i) {
				result.rotation[i] = nifly::Vector3(a_transform.rotation[i][0], a_transform.rotation[i][1], a_transform.rotation[i][2]);
			}
			result.translation = nifly::Vector3(a_transform.translation.x, a_transform.translation.y, a_transform.translation.z);
			result.scale = a_transform.scale;
			return result;
		}

		nifly::Vector3 V(Vec3 a_vector) { return nifly::Vector3(a_vector.x, a_vector.y, a_vector.z); }

		std::uint8_t UnitToByte(float a_value)
		{
			return static_cast<std::uint8_t>(std::clamp(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f), 0L, 255L));
		}

		class Writer
		{
		public:
			explicit Writer(nifly::NifFile& a_nif) :
				_nif(a_nif),
				_header(a_nif.GetHeader()),
				_version(a_nif.GetHeader().GetVersion())
			{}

			std::uint32_t AddNode(const OutNode& a_node, bool a_isRoot)
			{
				std::unique_ptr<nifly::NiNode> block;
				nifly::BSMultiBoundNode* multiBoundNode = nullptr;
				switch (a_node.type) {
				case NodeType::BSFadeNode:
					block = std::make_unique<nifly::BSFadeNode>();
					break;
				case NodeType::BSMultiBoundNode: {
					auto node = std::make_unique<nifly::BSMultiBoundNode>();
					multiBoundNode = node.get();
					block = std::move(node);
					break;
				}
				default:
					block = std::make_unique<nifly::NiNode>();
					break;
				}
				block->name.get() = a_node.name;
				block->flags = a_node.flags;
				block->SetTransformToParent(ToNifly(a_node.transform));

				std::uint32_t id;
				if (a_isRoot) {
					// NifFile::Create made block 0 a plain NiNode; the root takes its place.
					id = _header.ReplaceBlock(0, std::move(block));
				} else {
					id = _header.AddBlock(std::move(block));
				}
				auto* node = _header.GetBlock<nifly::NiNode>(id);

				if (multiBoundNode) {
					multiBoundNode = static_cast<nifly::BSMultiBoundNode*>(node);
					multiBoundNode->cullingMode = static_cast<nifly::BSCPCullingType>(a_node.cullingMode);
					if (a_node.multiBound) {
						auto aabb = std::make_unique<nifly::BSMultiBoundAABB>();
						aabb->center = V(a_node.multiBound->center);
						aabb->halfExtent = V(a_node.multiBound->halfExtent);
						const auto aabbId = _header.AddBlock(std::move(aabb));
						auto bound = std::make_unique<nifly::BSMultiBound>();
						bound->dataRef.index = aabbId;
						const auto boundId = _header.AddBlock(std::move(bound));
						static_cast<nifly::BSMultiBoundNode*>(_header.GetBlock<nifly::NiNode>(id))->multiBoundRef.index = boundId;
					}
				}

				for (const auto& shape : a_node.shapes) {
					const auto shapeId = AddShape(shape);
					_header.GetBlock<nifly::NiNode>(id)->childRefs.AddBlockRef(shapeId);
				}
				for (const auto& child : a_node.children) {
					const auto childId = AddNode(child, false);
					_header.GetBlock<nifly::NiNode>(id)->childRefs.AddBlockRef(childId);
				}
				return id;
			}

		private:
			std::uint32_t AddShape(const OutShape& a_shape)
			{
				if (a_shape.positions.size() > (std::numeric_limits<std::uint16_t>::max)()) {
					throw std::runtime_error("shape '" + a_shape.name + "' has more than 65535 vertices");
				}

				std::vector<nifly::Vector3> positions;
				positions.reserve(a_shape.positions.size());
				for (const auto& p : a_shape.positions) positions.push_back(V(p));
				std::vector<nifly::Triangle> triangles;
				triangles.reserve(a_shape.indices.size() / 3);
				for (std::size_t i = 0; i + 2 < a_shape.indices.size(); i += 3) {
					triangles.emplace_back(a_shape.indices[i], a_shape.indices[i + 1], a_shape.indices[i + 2]);
				}
				std::vector<nifly::Vector2> uvs;
				uvs.reserve(a_shape.uvs.size());
				for (const auto& uv : a_shape.uvs) uvs.emplace_back(uv.x, uv.y);
				std::vector<nifly::Vector3> normals;
				normals.reserve(a_shape.normals.size());
				for (const auto& n : a_shape.normals) normals.push_back(V(n));

				std::unique_ptr<nifly::BSTriShape> block;
				if (a_shape.subIndex) {
					block = std::make_unique<nifly::BSSubIndexTriShape>();
				} else {
					block = std::make_unique<nifly::BSTriShape>();
				}
				// BSTriShape::Create, not the sub-index override: that one marks the shape skinned.
				{
					TW_ZONE("nif shape create");
					block->BSTriShape::Create(_version, &positions, &triangles, uvs.empty() ? nullptr : &uvs, normals.empty() ? nullptr : &normals);
				}
				block->SetSkinned(false);
				block->SetUVs(!uvs.empty());
				if (!a_shape.tangents.empty() && a_shape.tangents.size() == a_shape.positions.size() &&
					a_shape.bitangents.size() == a_shape.positions.size()) {
					std::vector<nifly::Vector3> tangents, bitangents;
					for (const auto& t : a_shape.tangents) tangents.push_back(V(t));
					for (const auto& b : a_shape.bitangents) bitangents.push_back(V(b));
					block->SetTangentData(tangents);
					block->SetBitangentData(bitangents);
				}
				if (!a_shape.colors.empty() && a_shape.colors.size() == a_shape.positions.size()) {
					block->SetVertexColors(true);
					for (std::size_t i = 0; i < a_shape.colors.size(); ++i) {
						const auto& c = a_shape.colors[i];
						auto& data = block->vertData[i].colorData;
						data[0] = UnitToByte(c.x);
						data[1] = UnitToByte(c.y);
						data[2] = UnitToByte(c.z);
						data[3] = UnitToByte(c.w);
					}
				} else {
					block->SetVertexColors(false);
				}
				block->name.get() = a_shape.name;
				block->flags = 14;
				block->SetTransformToParent(ToNifly(a_shape.transform));
				// Create has the bounds already, from the same positions.

				if (a_shape.subIndex) {
					std::vector<nifly::BSGeometrySegmentData> segments;
					segments.reserve(a_shape.segments.size());
					for (const auto& segment : a_shape.segments) {
						nifly::BSGeometrySegmentData data;
						data.index = segment.startIndex;
						data.numTris = segment.triangleCount;
						segments.push_back(data);
					}
					static_cast<nifly::BSSubIndexTriShape*>(block.get())->SetSegments(segments);
				}

				const auto shapeId = _header.AddBlock(std::move(block));
				auto* shape = _header.GetBlock<nifly::BSTriShape>(shapeId);

				if (a_shape.shader.kind == ShaderKind::Lighting) {
					auto textures = std::make_unique<nifly::BSShaderTextureSet>(_version);
					for (std::uint32_t slot = 0; slot < kTextureSlots && slot < textures->textures.size(); ++slot) {
						textures->textures[slot].get() = a_shape.shader.textures[slot];
					}
					const auto texturesId = _header.AddBlock(std::move(textures));

					auto lighting = std::make_unique<nifly::BSLightingShaderProperty>();
					lighting->bBSLightingShaderProperty = true;
					lighting->SetShaderType(a_shape.shader.shaderType);
					lighting->shaderFlags1 = a_shape.shader.flags1;
					lighting->shaderFlags2 = a_shape.shader.flags2;
					lighting->uvOffset = nifly::Vector2(a_shape.shader.uvOffset.x, a_shape.shader.uvOffset.y);
					lighting->uvScale = nifly::Vector2(a_shape.shader.uvScale.x, a_shape.shader.uvScale.y);
					lighting->textureClampMode = static_cast<nifly::TexClampMode>(a_shape.shader.textureClampMode);
					lighting->alpha = a_shape.shader.alpha;
					lighting->glossiness = a_shape.shader.glossiness;
					lighting->emissiveColor = V(a_shape.shader.emissiveColor);
					lighting->emissiveMultiple = a_shape.shader.emissiveMultiple;
					lighting->textureSetRef.index = texturesId;
					const auto shaderId = _header.AddBlock(std::move(lighting));
					shape->ShaderPropertyRef()->index = shaderId;
				}
				if (a_shape.shader.alphaTest || a_shape.shader.alphaBlend) {
					auto alpha = std::make_unique<nifly::NiAlphaProperty>();
					alpha->flags = a_shape.shader.alphaFlags != 0 ? a_shape.shader.alphaFlags : static_cast<std::uint16_t>(4844);
					alpha->threshold = a_shape.shader.alphaThreshold;
					const auto alphaId = _header.AddBlock(std::move(alpha));
					_header.GetBlock<nifly::BSTriShape>(shapeId)->AlphaPropertyRef()->index = alphaId;
				}
				if (a_shape.largeReference) {
					auto extra = std::make_unique<nifly::BSDistantObjectLargeRefExtraData>();
					extra->name.get() = "DOLRED";
					extra->largeRef = true;
					const auto extraId = _header.AddBlock(std::move(extra));
					_header.GetBlock<nifly::BSTriShape>(shapeId)->extraDataRefs.AddBlockRef(extraId);
				}
				return shapeId;
			}

			nifly::NifFile& _nif;
			nifly::NiHeader& _header;
			nifly::NiVersion _version;
		};
	}

	bool WriteNifWithNifly(const OutNode& a_root, std::vector<std::uint8_t>& a_output, std::string* a_error)
	{
		TW_ZONE("WriteNif (nifly)");
		try {
			nifly::NifFile nif;
			nif.Create(nifly::NiVersion::getSSE());
			{
				TW_ZONE("nif build");
				Writer writer(nif);
				writer.AddNode(a_root, true);
			}

			std::ostringstream stream(std::ios::binary);
			nifly::NifSaveOptions options;
			options.optimize = false;
			options.sortBlocks = false;
			TW_ZONE("nif save");
			if (nif.Save(stream, options) != 0) {
				if (a_error) *a_error = "nifly failed to write the NIF";
				return false;
			}
			const auto bytes = std::move(stream).str();
			a_output.assign(bytes.begin(), bytes.end());
			return true;
		} catch (const std::exception& ex) {
			if (a_error) *a_error = ex.what();
			return false;
		}
	}
}
