#include "tesworld/Nif.h"

#include "tesworld/Profiling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <string_view>
#include <type_traits>
#include <unordered_map>

// nifly's Miniball and half-float conversion: the bounds and texture coordinates come out as nifly's own would.
#include <Miniball.hpp>
#include <half.hpp>

// The NIF is serialized here directly, byte for byte what nifly's NifFile::Save makes of the blocks the earlier writer
// built (detail::WriteNifWithNifly, which the tests hold this to). A shape is what BSTriShape::Create makes of its
// arrays, with what that writer set after it; its tangents and bounds take the same float operations in the same order,
// so they match to the bit.
namespace tesworld::nif
{
	namespace
	{
		enum class Block : std::uint8_t
		{
			NiNode,
			BSFadeNode,
			BSMultiBoundNode,
			BSMultiBoundAABB,
			BSMultiBound,
			BSTriShape,
			BSSubIndexTriShape,
			BSShaderTextureSet,
			BSLightingShaderProperty,
			NiAlphaProperty,
			BSDistantObjectLargeRefExtraData,
			Count
		};

		constexpr std::array<std::string_view, static_cast<std::size_t>(Block::Count)> kBlockNames{
			"NiNode",
			"BSFadeNode",
			"BSMultiBoundNode",
			"BSMultiBoundAABB",
			"BSMultiBound",
			"BSTriShape",
			"BSSubIndexTriShape",
			"BSShaderTextureSet",
			"BSLightingShaderProperty",
			"NiAlphaProperty",
			"BSDistantObjectLargeRefExtraData",
		};

		constexpr std::string_view kVersionLine = "Gamebryo File Format, Version 20.2.0.7\n";
		constexpr std::uint32_t kFileVersion = 0x14020007;
		constexpr std::uint32_t kUserVersion = 12;
		constexpr std::uint32_t kStreamVersion = 100;
		constexpr std::uint32_t kNoRef = 0xFFFFFFFFu;
		constexpr std::size_t kMaxVertices = (std::numeric_limits<std::uint16_t>::max)();
		// Create keeps at most this many triangles on SSE, whose triangle count is 16-bit.
		constexpr std::size_t kMaxTriangles = (std::numeric_limits<std::uint16_t>::max)();
		// The texture set of a Skyrim SE shader has nine slots, as Nif.h's kTextureSlots.
		constexpr std::size_t kTextureSetSize = 9;
		constexpr std::string_view kLargeRefName = "DOLRED";

		// nifly's VertexFlags.
		constexpr std::uint16_t kVertexPosition = 1 << 0;
		constexpr std::uint16_t kVertexUv = 1 << 1;
		constexpr std::uint16_t kVertexNormal = 1 << 3;
		constexpr std::uint16_t kVertexTangent = 1 << 4;
		constexpr std::uint16_t kVertexColors = 1 << 5;

		static_assert(sizeof(Vec3) == 3 * sizeof(float) && std::is_standard_layout_v<Vec3>);
		static_assert(sizeof(half_float::half) == 2);

		// nifly's Vector3, with the operations tangent space needs, written as nifly writes them.
		struct Vector
		{
			float x{ 0.0f };
			float y{ 0.0f };
			float z{ 0.0f };

			[[nodiscard]] bool IsZero() const { return x == 0.0f && y == 0.0f && z == 0.0f; }

			void Normalize()
			{
				float d = std::sqrt(x * x + y * y + z * z);
				if (d == 0.0f)
					d = 1.0f;
				x /= d;
				y /= d;
				z /= d;
			}

			Vector& operator+=(const Vector& a_other)
			{
				x += a_other.x;
				y += a_other.y;
				z += a_other.z;
				return *this;
			}

			[[nodiscard]] Vector operator-(const Vector& a_other) const { return { x - a_other.x, y - a_other.y, z - a_other.z }; }
			[[nodiscard]] Vector operator*(float a_value) const { return { x * a_value, y * a_value, z * a_value }; }
			[[nodiscard]] float Dot(const Vector& a_other) const { return x * a_other.x + y * a_other.y + z * a_other.z; }

			[[nodiscard]] Vector Cross(const Vector& a_other) const
			{
				Vector result;
				result.x = y * a_other.z - z * a_other.y;
				result.y = z * a_other.x - x * a_other.z;
				result.z = x * a_other.y - y * a_other.x;
				return result;
			}
		};

		// A unit vector's component as a byte, as BSTriShape stores normals and tangents.
		std::uint8_t UnitVectorByte(float a_value)
		{
			return static_cast<std::uint8_t>(std::round((((a_value + 1.0f) / 2.0f) * 255.0f)));
		}

		float UnitVectorFromByte(std::uint8_t a_value)
		{
			return ((static_cast<float>(a_value)) / 255.0f) * 2.0f - 1.0f;
		}

		std::uint8_t UnitToByte(float a_value)
		{
			return static_cast<std::uint8_t>(std::clamp(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f), 0L, 255L));
		}

		bool HasMultiBound(const OutNode& a_node)
		{
			return a_node.type == NodeType::BSMultiBoundNode && a_node.multiBound.has_value();
		}

		Block NodeBlock(NodeType a_type)
		{
			switch (a_type) {
			case NodeType::BSFadeNode:
				return Block::BSFadeNode;
			case NodeType::BSMultiBoundNode:
				return Block::BSMultiBoundNode;
			default:
				return Block::NiNode;
			}
		}

		bool HasLighting(const OutShape& a_shape) { return a_shape.shader.kind == ShaderKind::Lighting; }
		bool HasAlpha(const OutShape& a_shape) { return a_shape.shader.alphaTest || a_shape.shader.alphaBlend; }

		// The shape and the blocks that follow it: texture set and shader, alpha, extra data.
		std::uint32_t ShapeBlocks(const OutShape& a_shape)
		{
			return 1 + (HasLighting(a_shape) ? 2u : 0u) + (HasAlpha(a_shape) ? 1u : 0u) + (a_shape.largeReference ? 1u : 0u);
		}

		std::uint32_t NodeBlocks(const OutNode& a_node)
		{
			std::uint32_t count = 1 + (HasMultiBound(a_node) ? 2u : 0u);
			for (const auto& shape : a_node.shapes) count += ShapeBlocks(shape);
			for (const auto& child : a_node.children) count += NodeBlocks(child);
			return count;
		}

		// What a shape's vertices hold, from the arrays it was given. BSTriShape's constructor sets the position, UV,
		// normal, tangent and skinned flags; Create drops UVs whose count is wrong, drops normals and tangents without
		// normals, and computes tangent space when there are normals; the writer then dropped skinning, set UVs when there
		// were any, and set tangents and colours when they were given.
		struct ShapeLayout
		{
			std::uint32_t vertices{ 0 };
			std::uint32_t triangles{ 0 };
			std::uint16_t flags{ 0 };
			std::uint32_t vertexSize{ 0 };
			std::uint64_t description{ 0 };
			bool normals{ false };
			// UVs, one a vertex; without them a vertex's UV is zero.
			bool uvs{ false };
			bool tangentSpace{ false };
			bool givenTangents{ false };
			bool colors{ false };

			[[nodiscard]] std::uint32_t DataSize() const { return vertexSize * vertices + 6 * triangles; }
		};

		ShapeLayout Layout(const OutShape& a_shape)
		{
			ShapeLayout layout;
			const auto count = a_shape.positions.size();
			layout.vertices = static_cast<std::uint32_t>(count);
			layout.triangles = count == 0 ? 0 : static_cast<std::uint32_t>((std::min)(a_shape.indices.size() / 3, kMaxTriangles));
			layout.normals = !a_shape.normals.empty() && a_shape.normals.size() == count;
			layout.uvs = !a_shape.uvs.empty() && a_shape.uvs.size() == count;
			// Create left the constructor's UV flag on when it was given no UVs, so tangent space was computed then
			// too, from zero UVs.
			const bool uvFlagInCreate = a_shape.uvs.empty() || a_shape.uvs.size() == count;
			layout.tangentSpace = layout.normals && uvFlagInCreate;
			layout.givenTangents = !a_shape.tangents.empty() && a_shape.tangents.size() == count && a_shape.bitangents.size() == count;
			layout.colors = !a_shape.colors.empty() && a_shape.colors.size() == count;

			layout.flags = kVertexPosition;
			if (!a_shape.uvs.empty()) layout.flags |= kVertexUv;
			if (layout.normals) layout.flags |= kVertexNormal;
			if (layout.normals || layout.givenTangents) layout.flags |= kVertexTangent;
			if (layout.colors) layout.flags |= kVertexColors;

			// BSTriShape::CalcDataSizes and VertexDesc: attribute sizes in floats, each attribute's offset in 4-byte
			// units in its nibble of the description, positions at full precision on SSE, the flags on top.
			std::array<std::uint32_t, 9> sizes{};
			sizes[0] = 4;
			if (layout.flags & kVertexUv) sizes[1] = 1;
			if (layout.flags & kVertexNormal) {
				sizes[3] = 1;
				if (layout.flags & kVertexTangent) sizes[4] = 1;
			}
			if (layout.flags & kVertexColors) sizes[5] = 1;

			std::uint64_t description = static_cast<std::uint64_t>(layout.flags) << 44;
			std::uint32_t size = 0;
			for (std::uint32_t attribute = 0; attribute < sizes.size(); ++attribute) {
				if (sizes[attribute] == 0) continue;
				if (attribute != 0) {
					const std::uint64_t offset = static_cast<std::uint64_t>(size) << (4 * attribute + 2);
					const std::uint64_t rest = description & ~(static_cast<std::uint64_t>(15) << (4 * attribute + 4));
					description = offset | rest;
				}
				size += sizes[attribute] * 4;
			}
			description &= 0xFFFFFFFFFFFFFFF0ull;
			description |= static_cast<std::uint64_t>(size) >> 2;
			layout.vertexSize = size;
			layout.description = description;
			return layout;
		}

		// Block sizes in bytes, to reserve the output with.
		constexpr std::size_t kObjectNetSize = 12;
		constexpr std::size_t kAvObjectSize = 60;

		std::size_t NodeSize(const OutNode& a_node)
		{
			const std::size_t children = a_node.shapes.size() + a_node.children.size();
			return kObjectNetSize + kAvObjectSize + 8 + 4 * children + (a_node.type == NodeType::BSMultiBoundNode ? 8 : 0);
		}

		std::size_t LightingParametersSize(std::uint32_t a_shaderType)
		{
			switch (a_shaderType) {
			case 1:
				return 4;
			case 5:
			case 6:
				return 12;
			case 7:
				return 8;
			case 11:
				return 20;
			case 14:
				return 16;
			case 16:
				return 28;
			default:
				return 0;
			}
		}

		std::size_t ShapeBlocksSize(const OutShape& a_shape, const ShapeLayout& a_layout)
		{
			std::size_t size = kObjectNetSize + (a_shape.largeReference ? 4 : 0) + 108 + a_layout.DataSize();
			if (a_shape.subIndex) size += 4 + 9 * a_shape.segments.size();
			if (HasLighting(a_shape)) {
				size += 4;
				for (std::size_t slot = 0; slot < kTextureSetSize; ++slot) size += 4 + a_shape.shader.textures[slot].size();
				size += 4 + kObjectNetSize + 24 + 60 + LightingParametersSize(a_shape.shader.shaderType);
			}
			if (HasAlpha(a_shape)) size += kObjectNetSize + 3;
			if (a_shape.largeReference) size += 5;
			return size;
		}

		constexpr std::uint16_t kNoType = 0xFFFF;

		// Kept by each thread across calls, so a steady stream of files allocates nothing a vertex.
		struct Scratch
		{
			std::vector<std::uint16_t> blockTypes;
			std::array<std::uint16_t, static_cast<std::size_t>(Block::Count)> typeIndex{};
			std::vector<Block> types;
			std::vector<std::string_view> strings;
			std::unordered_map<std::string_view, std::uint32_t> stringIds;
			std::vector<const float*> points;
			std::vector<Vector> tangents;
			std::vector<Vector> bitangents;
		};

		thread_local Scratch t_scratch;

		// The header's tables, in the order nifly fills them: block types as they first appear, and the names of the
		// blocks that have one, each once, empty names left out (their references are -1).
		class Planner
		{
		public:
			explicit Planner(Scratch& a_scratch) :
				_scratch(a_scratch)
			{
				_scratch.blockTypes.clear();
				_scratch.typeIndex.fill(kNoType);
				_scratch.types.clear();
				_scratch.strings.clear();
				_scratch.stringIds.clear();
			}

			// The names are views into this call's tree, which the caller may free before the next call; the table is
			// emptied while they're valid, on every way out of WriteNif, since clearing a map can hash its keys again
			// (MSVC's does).
			~Planner()
			{
				_scratch.stringIds.clear();
				_scratch.strings.clear();
			}

			Planner(const Planner&) = delete;
			Planner& operator=(const Planner&) = delete;

			// False when a shape has too many vertices; nifly's writer stopped at the first.
			bool AddNode(const OutNode& a_node)
			{
				AddBlock(NodeBlock(a_node.type), NodeSize(a_node));
				AddString(a_node.name);
				if (HasMultiBound(a_node)) {
					AddBlock(Block::BSMultiBoundAABB, 24);
					AddBlock(Block::BSMultiBound, 4);
				}
				for (const auto& shape : a_node.shapes) {
					if (shape.positions.size() > kMaxVertices) {
						_tooLarge = &shape;
						return false;
					}
					AddBlock(shape.subIndex ? Block::BSSubIndexTriShape : Block::BSTriShape, ShapeBlocksSize(shape, Layout(shape)));
					AddString(shape.name);
					if (HasLighting(shape)) {
						AddBlock(Block::BSShaderTextureSet, 0);
						AddBlock(Block::BSLightingShaderProperty, 0);
					}
					if (HasAlpha(shape)) AddBlock(Block::NiAlphaProperty, 0);
					if (shape.largeReference) {
						AddBlock(Block::BSDistantObjectLargeRefExtraData, 0);
						AddString(kLargeRefName);
					}
				}
				for (const auto& child : a_node.children) {
					if (!AddNode(child)) return false;
				}
				return true;
			}

			[[nodiscard]] const OutShape* TooLarge() const { return _tooLarge; }

			[[nodiscard]] std::size_t FileSize() const
			{
				std::size_t size = kVersionLine.size() + 4 + 1 + 4 + 4 + 4 + 6 + 2;
				for (const auto type : _scratch.types) size += 4 + kBlockNames[static_cast<std::size_t>(type)].size();
				size += 6 * _scratch.blockTypes.size() + 8;
				for (const auto string : _scratch.strings) size += 4 + string.size();
				return size + 4 + _blocks + 8;
			}

		private:
			void AddBlock(Block a_type, std::size_t a_size)
			{
				auto& index = _scratch.typeIndex[static_cast<std::size_t>(a_type)];
				if (index == kNoType) {
					index = static_cast<std::uint16_t>(_scratch.types.size());
					_scratch.types.push_back(a_type);
				}
				_scratch.blockTypes.push_back(index);
				_blocks += a_size;
			}

			void AddString(std::string_view a_string)
			{
				if (a_string.empty()) return;
				const auto [it, added] = _scratch.stringIds.try_emplace(a_string, static_cast<std::uint32_t>(_scratch.strings.size()));
				if (added) _scratch.strings.push_back(a_string);
			}

			Scratch& _scratch;
			std::size_t _blocks{ 0 };
			const OutShape* _tooLarge{ nullptr };
		};

		// Appends to the output vector, sized for the whole file up front.
		class Output
		{
		public:
			Output(std::vector<std::uint8_t>& a_bytes, std::size_t a_expected) :
				_bytes(a_bytes)
			{
				_bytes.clear();
				_bytes.resize(a_expected);
			}

			std::uint8_t* Take(std::size_t a_count)
			{
				if (_size + a_count > _bytes.size()) {
					_bytes.resize((std::max)(_size + a_count, _bytes.size() * 2));
				}
				auto* data = _bytes.data() + _size;
				_size += a_count;
				return data;
			}

			template <class T>
			void Put(const T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				std::memcpy(Take(sizeof(T)), &a_value, sizeof(T));
			}

			void PutBytes(const void* a_data, std::size_t a_count)
			{
				if (a_count != 0) std::memcpy(Take(a_count), a_data, a_count);
			}

			// A string with a 32-bit length and no terminator.
			void PutString(std::string_view a_string)
			{
				Put(static_cast<std::uint32_t>(a_string.size()));
				PutBytes(a_string.data(), a_string.size());
			}

			void PutVector(Vec3 a_vector)
			{
				Put(a_vector.x);
				Put(a_vector.y);
				Put(a_vector.z);
			}

			void Patch(std::size_t a_offset, std::uint32_t a_value) { std::memcpy(_bytes.data() + a_offset, &a_value, sizeof(a_value)); }

			[[nodiscard]] std::size_t Size() const { return _size; }

			void Finish() { _bytes.resize(_size); }

		private:
			std::vector<std::uint8_t>& _bytes;
			std::size_t _size{ 0 };
		};

		class Writer
		{
		public:
			Writer(Output& a_output, Scratch& a_scratch) :
				_out(a_output),
				_scratch(a_scratch)
			{}

			void Header()
			{
				const auto blocks = static_cast<std::uint32_t>(_scratch.blockTypes.size());
				_out.PutBytes(kVersionLine.data(), kVersionLine.size());
				_out.Put(kFileVersion);
				_out.Put(std::uint8_t{ 1 });  // little endian
				_out.Put(kUserVersion);
				_out.Put(blocks);
				_out.Put(kStreamVersion);
				// The creator and two export strings, empty: a length byte that counts the null after it.
				for (int i = 0; i < 3; ++i) {
					_out.Put(std::uint8_t{ 1 });
					_out.Put(std::uint8_t{ 0 });
				}
				_out.Put(static_cast<std::uint16_t>(_scratch.types.size()));
				for (const auto type : _scratch.types) _out.PutString(kBlockNames[static_cast<std::size_t>(type)]);
				_out.PutBytes(_scratch.blockTypes.data(), _scratch.blockTypes.size() * sizeof(std::uint16_t));
				// Block sizes, filled in as the blocks are written.
				_blockSizes = _out.Size();
				std::memset(_out.Take(4 * std::size_t{ blocks }), 0, 4 * std::size_t{ blocks });

				std::uint32_t longest = 0;
				for (const auto string : _scratch.strings) longest = (std::max)(longest, static_cast<std::uint32_t>(string.size()));
				_out.Put(static_cast<std::uint32_t>(_scratch.strings.size()));
				_out.Put(longest);
				for (const auto string : _scratch.strings) _out.PutString(string);
				_out.Put(std::uint32_t{ 0 });  // groups
			}

			void Footer()
			{
				// One root, the first block.
				_out.Put(std::uint32_t{ 1 });
				_out.Put(std::uint32_t{ 0 });
			}

			// Writes the node's blocks, then its shapes' and its children's; returns the next free block index.
			std::uint32_t Node(const OutNode& a_node, std::uint32_t a_id)
			{
				const bool multiBound = HasMultiBound(a_node);
				const std::uint32_t firstChild = a_id + 1 + (multiBound ? 2u : 0u);

				Begin(a_id);
				ObjectNet(StringId(a_node.name), kNoRef);
				_out.Put(a_node.flags);
				PutTransform(a_node.transform);
				_out.Put(kNoRef);  // collision
				_out.Put(static_cast<std::uint32_t>(a_node.shapes.size() + a_node.children.size()));
				std::uint32_t id = firstChild;
				for (const auto& shape : a_node.shapes) {
					_out.Put(id);
					id += ShapeBlocks(shape);
				}
				for (const auto& child : a_node.children) {
					_out.Put(id);
					id += NodeBlocks(child);
				}
				_out.Put(std::uint32_t{ 0 });  // effects
				if (a_node.type == NodeType::BSMultiBoundNode) {
					_out.Put(multiBound ? a_id + 2 : kNoRef);
					_out.Put(a_node.cullingMode);
				}
				End();

				if (multiBound) {
					Begin(a_id + 1);
					_out.PutVector(a_node.multiBound->center);
					_out.PutVector(a_node.multiBound->halfExtent);
					End();
					Begin(a_id + 2);
					_out.Put(a_id + 1);
					End();
				}

				id = firstChild;
				for (const auto& shape : a_node.shapes) id = Shape(shape, id);
				for (const auto& child : a_node.children) id = Node(child, id);
				return id;
			}

		private:
			void Begin(std::uint32_t a_id)
			{
				_block = a_id;
				_blockStart = _out.Size();
			}

			void End() { _out.Patch(_blockSizes + 4 * std::size_t{ _block }, static_cast<std::uint32_t>(_out.Size() - _blockStart)); }

			[[nodiscard]] std::uint32_t StringId(std::string_view a_string) const
			{
				if (a_string.empty()) return kNoRef;
				return _scratch.stringIds.find(a_string)->second;
			}

			// NiObjectNET: the name, extra data (one block or none) and no controller.
			void ObjectNet(std::uint32_t a_name, std::uint32_t a_extraData)
			{
				_out.Put(a_name);
				if (a_extraData == kNoRef) {
					_out.Put(std::uint32_t{ 0 });
				} else {
					_out.Put(std::uint32_t{ 1 });
					_out.Put(a_extraData);
				}
				_out.Put(kNoRef);
			}

			void PutTransform(const Transform& a_transform)
			{
				_out.PutVector(a_transform.translation);
				for (const auto& row : a_transform.rotation) {
					for (const float value : row) _out.Put(value);
				}
				_out.Put(a_transform.scale);
			}

			std::uint32_t Shape(const OutShape& a_shape, std::uint32_t a_id)
			{
				const auto layout = Layout(a_shape);
				std::uint32_t next = a_id + 1;
				std::uint32_t textures = kNoRef;
				std::uint32_t shader = kNoRef;
				std::uint32_t alpha = kNoRef;
				std::uint32_t extraData = kNoRef;
				if (HasLighting(a_shape)) {
					textures = next++;
					shader = next++;
				}
				if (HasAlpha(a_shape)) alpha = next++;
				if (a_shape.largeReference) extraData = next++;

				Begin(a_id);
				ObjectNet(StringId(a_shape.name), extraData);
				_out.Put(std::uint32_t{ 14 });  // flags
				PutTransform(a_shape.transform);
				_out.Put(kNoRef);  // collision
				Bounds(a_shape);
				_out.Put(kNoRef);  // skin
				_out.Put(shader);
				_out.Put(alpha);
				_out.Put(layout.description);
				_out.Put(static_cast<std::uint16_t>(layout.triangles));
				_out.Put(static_cast<std::uint16_t>(layout.vertices));
				_out.Put(layout.DataSize());
				if (layout.DataSize() > 0) {
					Vertices(a_shape, layout);
					_out.PutBytes(a_shape.indices.data(), 6 * std::size_t{ layout.triangles });
				}
				_out.Put(std::uint32_t{ 0 });  // particle data
				if (a_shape.subIndex) {
					_out.Put(static_cast<std::uint32_t>(a_shape.segments.size()));
					for (const auto& segment : a_shape.segments) {
						_out.Put(std::uint8_t{ 0 });  // flags
						_out.Put(segment.startIndex);
						_out.Put(segment.triangleCount);
					}
				}
				End();

				const auto& data = a_shape.shader;
				if (HasLighting(a_shape)) {
					Begin(textures);
					_out.Put(static_cast<std::uint32_t>(kTextureSetSize));
					for (std::size_t slot = 0; slot < kTextureSetSize; ++slot) _out.PutString(data.textures[slot]);
					End();

					Begin(shader);
					// NiObjectNET leads with the shader type in a lighting shader.
					_out.Put(data.shaderType);
					ObjectNet(kNoRef, kNoRef);
					_out.Put(data.flags1);
					_out.Put(data.flags2);
					_out.Put(data.uvOffset);
					_out.Put(data.uvScale);
					_out.Put(textures);
					_out.PutVector(data.emissiveColor);
					_out.Put(data.emissiveMultiple);
					_out.Put(data.textureClampMode);
					_out.Put(data.alpha);
					_out.Put(data.refractionStrength);
					_out.Put(data.glossiness);
					_out.PutVector(data.specularColor);
					_out.Put(data.specularStrength);
					_out.Put(data.softLighting);
					_out.Put(data.rimLightPower);
					// Preserve the source parameters, including fields repurposed by PBR (docs/systems/lod-formats.md).
					switch (data.shaderType) {
					case 1:
						_out.Put(data.environmentMapScale);
						break;
					case 5:
						_out.PutVector(data.skinTintColor);
						break;
					case 6:
						_out.PutVector(data.hairTintColor);
						break;
					case 7:
						_out.Put(data.lightingMaxPasses);
						_out.Put(data.lightingScale);
						break;
					case 11:
						_out.Put(data.parallaxInnerLayerThickness);
						_out.Put(data.parallaxRefractionScale);
						_out.Put(data.parallaxInnerLayerTextureScale);
						_out.Put(data.parallaxEnvironmentMapStrength);
						break;
					case 14:
						_out.Put(data.sparkleParameters);
						break;
					case 16:
						_out.Put(data.eyeCubemapScale);
						_out.PutVector(data.eyeLeftReflectionCenter);
						_out.PutVector(data.eyeRightReflectionCenter);
						break;
					default:
						break;
					}
					End();
				}
				if (alpha != kNoRef) {
					Begin(alpha);
					ObjectNet(kNoRef, kNoRef);
					_out.Put(data.alphaFlags != 0 ? data.alphaFlags : static_cast<std::uint16_t>(4844));
					_out.Put(data.alphaThreshold);
					End();
				}
				if (extraData != kNoRef) {
					Begin(extraData);
					_out.Put(StringId(kLargeRefName));
					_out.Put(std::uint8_t{ 1 });  // large reference
					End();
				}
				return next;
			}

			// nifly's BoundingSphere: Miniball over the positions.
			void Bounds(const OutShape& a_shape)
			{
				if (a_shape.positions.empty()) {
					_out.PutVector({});
					_out.Put(0.0f);
					return;
				}
				auto& points = _scratch.points;
				points.resize(a_shape.positions.size());
				for (std::size_t i = 0; i < points.size(); ++i) points[i] = &a_shape.positions[i].x;
				Miniball::Miniball<Miniball::CoordAccessor<std::vector<const float*>::const_iterator, const float*>> ball(3, points.cbegin(), points.cend());
				const float* center = ball.center();
				_out.Put(center[0]);
				_out.Put(center[1]);
				_out.Put(center[2]);
				_out.Put(std::sqrt(ball.squared_radius()));
			}

			static Vec2 Uv(const OutShape& a_shape, const ShapeLayout& a_layout, std::size_t a_index)
			{
				return a_layout.uvs ? a_shape.uvs[a_index] : Vec2{};
			}

			// BSTriShape::CalcTangentSpace's first half: each triangle's directions summed at its corners.
			void AccumulateTangents(const OutShape& a_shape, const ShapeLayout& a_layout)
			{
				auto& tan1 = _scratch.tangents;
				auto& tan2 = _scratch.bitangents;
				tan1.assign(a_layout.vertices, Vector{});
				tan2.assign(a_layout.vertices, Vector{});
				const int vertices = static_cast<int>(a_layout.vertices);
				for (std::size_t triangle = 0; triangle < a_layout.triangles; ++triangle) {
					const int i1 = a_shape.indices[3 * triangle];
					const int i2 = a_shape.indices[3 * triangle + 1];
					const int i3 = a_shape.indices[3 * triangle + 2];
					if (i1 >= vertices || i2 >= vertices || i3 >= vertices) continue;

					const Vec3 v1 = a_shape.positions[i1];
					const Vec3 v2 = a_shape.positions[i2];
					const Vec3 v3 = a_shape.positions[i3];
					const Vec2 w1 = Uv(a_shape, a_layout, i1);
					const Vec2 w2 = Uv(a_shape, a_layout, i2);
					const Vec2 w3 = Uv(a_shape, a_layout, i3);

					const float x1 = v2.x - v1.x;
					const float x2 = v3.x - v1.x;
					const float y1 = v2.y - v1.y;
					const float y2 = v3.y - v1.y;
					const float z1 = v2.z - v1.z;
					const float z2 = v3.z - v1.z;

					const float s1 = w2.x - w1.x;
					const float s2 = w3.x - w1.x;
					const float t1 = w2.y - w1.y;
					const float t2 = w3.y - w1.y;

					float r = (s1 * t2 - s2 * t1);
					r = (r >= 0.0f ? +1.0f : -1.0f);

					Vector sdir{ (t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r, (t2 * z1 - t1 * z2) * r };
					Vector tdir{ (s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r, (s1 * z2 - s2 * z1) * r };

					sdir.Normalize();
					tdir.Normalize();

					tan1[i1] += tdir;
					tan1[i2] += tdir;
					tan1[i3] += tdir;

					tan2[i1] += sdir;
					tan2[i2] += sdir;
					tan2[i3] += sdir;
				}
			}

			void Vertices(const OutShape& a_shape, const ShapeLayout& a_layout)
			{
				if (a_layout.tangentSpace) AccumulateTangents(a_shape, a_layout);
				const bool uvs = (a_layout.flags & kVertexUv) != 0;
				const bool normals = (a_layout.flags & kVertexNormal) != 0;
				const bool tangents = normals && (a_layout.flags & kVertexTangent) != 0;
				const bool colors = (a_layout.flags & kVertexColors) != 0;

				std::uint8_t* out = _out.Take(std::size_t{ a_layout.vertexSize } * a_layout.vertices);
				for (std::size_t i = 0; i < a_layout.vertices; ++i) {
					std::uint8_t normal[3]{};
					std::uint8_t tangent[3]{};
					float bitangentX = 0.0f;
					std::uint8_t bitangentY = 0;
					std::uint8_t bitangentZ = 0;

					if (a_layout.normals) {
						const Vec3 n = a_shape.normals[i];
						normal[0] = UnitVectorByte(n.x);
						normal[1] = UnitVectorByte(n.y);
						normal[2] = UnitVectorByte(n.z);
					}
					if (a_layout.tangentSpace) {
						// CalcTangentSpace's second half, against the normal as stored.
						const Vector rawNormal{ UnitVectorFromByte(normal[0]), UnitVectorFromByte(normal[1]), UnitVectorFromByte(normal[2]) };
						Vector rawTangent = _scratch.tangents[i];
						Vector rawBitangent = _scratch.bitangents[i];
						if (rawTangent.IsZero() || rawBitangent.IsZero()) {
							rawTangent.x = rawNormal.y;
							rawTangent.y = rawNormal.z;
							rawTangent.z = rawNormal.x;
							rawBitangent = rawNormal.Cross(rawTangent);
						} else {
							rawTangent.Normalize();
							rawTangent = (rawTangent - rawNormal * rawNormal.Dot(rawTangent));
							rawTangent.Normalize();

							rawBitangent.Normalize();

							rawBitangent = (rawBitangent - rawNormal * rawNormal.Dot(rawBitangent));
							rawBitangent = (rawBitangent - rawTangent * rawTangent.Dot(rawBitangent));

							rawBitangent.Normalize();
						}
						tangent[0] = UnitVectorByte(rawTangent.x);
						tangent[1] = UnitVectorByte(rawTangent.y);
						tangent[2] = UnitVectorByte(rawTangent.z);
						bitangentX = rawBitangent.x;
						bitangentY = UnitVectorByte(rawBitangent.y);
						bitangentZ = UnitVectorByte(rawBitangent.z);
					}
					if (a_layout.givenTangents) {
						const Vec3 t = a_shape.tangents[i];
						const Vec3 b = a_shape.bitangents[i];
						tangent[0] = UnitVectorByte(t.x);
						tangent[1] = UnitVectorByte(t.y);
						tangent[2] = UnitVectorByte(t.z);
						bitangentX = b.x;
						bitangentY = UnitVectorByte(b.y);
						bitangentZ = UnitVectorByte(b.z);
					}

					// The position and the bitangent's x, at full precision on SSE.
					std::memcpy(out, &a_shape.positions[i], 12);
					std::memcpy(out + 12, &bitangentX, 4);
					out += 16;
					if (uvs) {
						const Vec2 uv = Uv(a_shape, a_layout, i);
						const half_float::half u(uv.x);
						const half_float::half v(uv.y);
						std::memcpy(out, &u, 2);
						std::memcpy(out + 2, &v, 2);
						out += 4;
					}
					if (normals) {
						out[0] = normal[0];
						out[1] = normal[1];
						out[2] = normal[2];
						out[3] = bitangentY;
						out += 4;
						if (tangents) {
							out[0] = tangent[0];
							out[1] = tangent[1];
							out[2] = tangent[2];
							out[3] = bitangentZ;
							out += 4;
						}
					}
					if (colors) {
						const Vec4 c = a_shape.colors[i];
						out[0] = UnitToByte(c.x);
						out[1] = UnitToByte(c.y);
						out[2] = UnitToByte(c.z);
						out[3] = UnitToByte(c.w);
						out += 4;
					}
				}
			}

			Output& _out;
			Scratch& _scratch;
			std::size_t _blockSizes{ 0 };
			std::uint32_t _block{ 0 };
			std::size_t _blockStart{ 0 };
		};
	}

	bool WriteNif(const OutNode& a_root, std::vector<std::uint8_t>& a_output, std::string* a_error)
	{
		TW_ZONE("WriteNif");
		try {
			auto& scratch = t_scratch;
			Planner planner(scratch);
			if (!planner.AddNode(a_root)) {
				if (a_error) *a_error = "shape '" + planner.TooLarge()->name + "' has more than 65535 vertices";
				return false;
			}

			Output output(a_output, planner.FileSize());
			Writer writer(output, scratch);
			writer.Header();
			writer.Node(a_root, 0);
			writer.Footer();
			output.Finish();
			return true;
		} catch (const std::exception& ex) {
			if (a_error) *a_error = ex.what();
			return false;
		}
	}
}