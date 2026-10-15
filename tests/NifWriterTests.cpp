// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Nif.h"

#include "NifWriterReference.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace tesworld;
using namespace tesworld::nif;

// WriteNif against the nifly writer it replaced: the same bytes for every tree. Run with --time to time both on a large
// tree.
namespace
{
	using Rng = std::mt19937;

	float Uniform(Rng& a_rng, float a_min, float a_max) { return std::uniform_real_distribution<float>(a_min, a_max)(a_rng); }
	int Int(Rng& a_rng, int a_min, int a_max) { return std::uniform_int_distribution<int>(a_min, a_max)(a_rng); }
	bool Chance(Rng& a_rng, double a_probability) { return std::bernoulli_distribution(a_probability)(a_rng); }

	Vec3 UnitVector(Rng& a_rng)
	{
		switch (Int(a_rng, 0, 5)) {
		case 0:
			return { 0.0f, 0.0f, 1.0f };
		case 1:
			return { -1.0f, 0.0f, 0.0f };
		default:
			break;
		}
		const Vec3 v{ Uniform(a_rng, -1.0f, 1.0f), Uniform(a_rng, -1.0f, 1.0f), Uniform(a_rng, -1.0f, 1.0f) };
		return Normalize(v);
	}

	// Names drawn from a small pool so they repeat, with empty ones, the root nifly's Create made and a long one.
	std::string Name(Rng& a_rng)
	{
		static const std::vector<std::string> names{ "", "", "obj", "objSnow", "water", "DOLRED", "Scene Root", "a",
			std::string(300, 'n') };
		return names[static_cast<std::size_t>(Int(a_rng, 0, static_cast<int>(names.size()) - 1))];
	}

	std::string Texture(Rng& a_rng)
	{
		static const std::vector<std::string> textures{ "", "textures\\lod\\rock.dds", "textures\\lod\\rock_n.dds",
			"textures\\terrain\\tamriel\\objects\\tamriel.objects_n.dds", std::string(260, 't') };
		return textures[static_cast<std::size_t>(Int(a_rng, 0, static_cast<int>(textures.size()) - 1))];
	}

	Transform RandomTransform(Rng& a_rng)
	{
		Transform transform;
		if (Chance(a_rng, 0.3)) return transform;
		for (auto& row : transform.rotation) {
			for (auto& value : row) value = Uniform(a_rng, -1.0f, 1.0f);
		}
		transform.translation = { Uniform(a_rng, -200000.0f, 200000.0f), Uniform(a_rng, -200000.0f, 200000.0f), Uniform(a_rng, -5000.0f, 5000.0f) };
		transform.scale = Uniform(a_rng, 0.1f, 8.0f);
		return transform;
	}

	// A size that matches the vertex count most of the time, or is empty, or (rarely) wrong.
	std::size_t ArraySize(Rng& a_rng, std::size_t a_vertices, double a_present)
	{
		if (!Chance(a_rng, a_present)) return 0;
		if (Chance(a_rng, 0.08)) return a_vertices + 1 + static_cast<std::size_t>(Int(a_rng, 0, 3));
		return a_vertices;
	}

	OutShape RandomShape(Rng& a_rng, std::size_t a_vertices)
	{
		OutShape shape;
		shape.name = Name(a_rng);
		shape.transform = RandomTransform(a_rng);

		const float extent = Chance(a_rng, 0.5) ? 4096.0f : 50.0f;
		const bool flat = Chance(a_rng, 0.15);
		const bool collapsed = Chance(a_rng, 0.05);
		for (std::size_t i = 0; i < a_vertices; ++i) {
			Vec3 p{ Uniform(a_rng, -extent, extent), Uniform(a_rng, -extent, extent), flat ? 7.5f : Uniform(a_rng, -extent, extent) };
			if (collapsed) p = { 1.0f, 2.0f, 3.0f };
			// Repeated positions, as shapes that share corners have.
			if (i > 0 && Chance(a_rng, 0.05)) p = shape.positions[static_cast<std::size_t>(Int(a_rng, 0, static_cast<int>(i) - 1))];
			shape.positions.push_back(p);
		}

		shape.normals.resize(ArraySize(a_rng, a_vertices, 0.75));
		for (auto& n : shape.normals) n = UnitVector(a_rng);

		shape.uvs.resize(ArraySize(a_rng, a_vertices, 0.7));
		const int uvKind = Int(a_rng, 0, 5);
		for (auto& uv : shape.uvs) {
			switch (uvKind) {
			case 0:  // zero area: the tangent fallback
				uv = { 0.25f, 0.75f };
				break;
			case 1:  // beyond what half floats hold, and below their normal range
				uv = { Uniform(a_rng, -80000.0f, 80000.0f), Uniform(a_rng, -1e-6f, 1e-6f) };
				break;
			case 2:  // tiled
				uv = { Uniform(a_rng, -40.0f, 40.0f), Uniform(a_rng, -40.0f, 40.0f) };
				break;
			default:
				uv = { Uniform(a_rng, 0.0f, 1.0f), Uniform(a_rng, 0.0f, 1.0f) };
				break;
			}
		}

		if (Chance(a_rng, 0.3)) {
			const bool wrong = Chance(a_rng, 0.1);
			shape.tangents.resize(a_vertices + (wrong ? 1 : 0));
			shape.bitangents.resize(a_vertices);
			for (auto& t : shape.tangents) t = UnitVector(a_rng);
			for (auto& b : shape.bitangents) b = UnitVector(a_rng);
		}

		shape.colors.resize(ArraySize(a_rng, a_vertices, 0.4));
		for (auto& c : shape.colors) c = { Uniform(a_rng, -0.5f, 1.5f), Uniform(a_rng, 0.0f, 1.0f), Uniform(a_rng, 0.0f, 1.0f), Uniform(a_rng, 0.0f, 1.0f) };

		const auto triangles = a_vertices == 0 ? static_cast<std::size_t>(Int(a_rng, 0, 2)) : static_cast<std::size_t>(Int(a_rng, 0, static_cast<int>(a_vertices) * 2));
		const int top = a_vertices == 0 ? 3 : static_cast<int>(a_vertices) - 1;
		for (std::size_t t = 0; t < triangles; ++t) {
			std::uint16_t a = static_cast<std::uint16_t>(Int(a_rng, 0, top));
			std::uint16_t b = static_cast<std::uint16_t>(Int(a_rng, 0, top));
			std::uint16_t c = static_cast<std::uint16_t>(Int(a_rng, 0, top));
			if (Chance(a_rng, 0.05)) b = a;  // degenerate
			if (Chance(a_rng, 0.02)) c = static_cast<std::uint16_t>(a_vertices + 5);  // out of range
			shape.indices.insert(shape.indices.end(), { a, b, c });
		}
		// A stray index past the last whole triangle.
		if (Chance(a_rng, 0.1)) shape.indices.push_back(0);

		shape.subIndex = Chance(a_rng, 0.4);
		if (shape.subIndex || Chance(a_rng, 0.1)) {
			const int segments = Int(a_rng, 0, 4);
			for (int s = 0; s < segments; ++s) {
				shape.segments.push_back({ static_cast<std::uint32_t>(Int(a_rng, 0, 3000)), static_cast<std::uint32_t>(Int(a_rng, 0, 1000)) });
			}
		}

		auto& shader = shape.shader;
		switch (Int(a_rng, 0, 5)) {
		case 0:
			shader.kind = ShaderKind::None;
			break;
		case 1:
			shader.kind = ShaderKind::Effect;
			shader.effectTexture = Texture(a_rng);
			break;
		case 2:
			shader.kind = ShaderKind::Other;
			break;
		default:
			shader.kind = ShaderKind::Lighting;
			break;
		}
		static const std::vector<std::uint32_t> shaderTypes{ 0, 1, 2, 3, 5, 6, 7, 11, 14, 16, 18, 19 };
		shader.shaderType = shaderTypes[static_cast<std::size_t>(Int(a_rng, 0, static_cast<int>(shaderTypes.size()) - 1))];
		shader.flags1 = static_cast<std::uint32_t>(a_rng());
		shader.flags2 = static_cast<std::uint32_t>(a_rng());
		for (auto& texture : shader.textures) texture = Texture(a_rng);
		if (Chance(a_rng, 0.5)) {
			shader.uvOffset = { Uniform(a_rng, -1.0f, 1.0f), Uniform(a_rng, -1.0f, 1.0f) };
			shader.uvScale = { Uniform(a_rng, 0.1f, 4.0f), Uniform(a_rng, 0.1f, 4.0f) };
		}
		shader.textureClampMode = static_cast<std::uint32_t>(Int(a_rng, 0, 3));
		shader.alpha = Uniform(a_rng, 0.0f, 1.0f);
		shader.glossiness = Uniform(a_rng, 1.0f, 100.0f);
		shader.emissiveColor = { Uniform(a_rng, 0.0f, 1.0f), Uniform(a_rng, 0.0f, 1.0f), Uniform(a_rng, 0.0f, 1.0f) };
		shader.emissiveMultiple = Uniform(a_rng, 0.0f, 4.0f);
		shader.alphaBlend = Chance(a_rng, 0.25);
		shader.alphaTest = Chance(a_rng, 0.35);
		shader.alphaThreshold = static_cast<std::uint8_t>(Int(a_rng, 0, 255));
		shader.alphaFlags = Chance(a_rng, 0.5) ? std::uint16_t{ 0 } : static_cast<std::uint16_t>(Int(a_rng, 1, 65535));
		shape.largeReference = Chance(a_rng, 0.3);
		return shape;
	}

	std::size_t VertexCount(Rng& a_rng)
	{
		switch (Int(a_rng, 0, 9)) {
		case 0:
			return 0;
		case 1:
			return static_cast<std::size_t>(Int(a_rng, 1, 3));
		case 2:
			return static_cast<std::size_t>(Int(a_rng, 500, 4000));
		default:
			return static_cast<std::size_t>(Int(a_rng, 4, 200));
		}
	}

	OutNode RandomNode(Rng& a_rng, int a_depth)
	{
		OutNode node;
		node.type = static_cast<NodeType>(Int(a_rng, 0, 2));
		node.name = Name(a_rng);
		node.transform = RandomTransform(a_rng);
		node.flags = Chance(a_rng, 0.5) ? 14u : static_cast<std::uint32_t>(a_rng());
		if (Chance(a_rng, 0.6)) {
			node.multiBound = AxisAlignedBox{ { Uniform(a_rng, -1e5f, 1e5f), Uniform(a_rng, -1e5f, 1e5f), Uniform(a_rng, -1e4f, 1e4f) },
				{ Uniform(a_rng, 0.0f, 1e4f), Uniform(a_rng, 0.0f, 1e4f), Uniform(a_rng, 0.0f, 1e4f) } };
		}
		node.cullingMode = static_cast<std::uint32_t>(Int(a_rng, 0, 4));
		const int shapes = Int(a_rng, 0, 3);
		for (int i = 0; i < shapes; ++i) node.shapes.push_back(RandomShape(a_rng, VertexCount(a_rng)));
		if (a_depth < 3) {
			const int children = Int(a_rng, 0, a_depth == 0 ? 3 : 2);
			for (int i = 0; i < children; ++i) node.children.push_back(RandomNode(a_rng, a_depth + 1));
		}
		return node;
	}

	std::size_t FirstDifference(const std::vector<std::uint8_t>& a_left, const std::vector<std::uint8_t>& a_right)
	{
		const auto count = (std::min)(a_left.size(), a_right.size());
		for (std::size_t i = 0; i < count; ++i) {
			if (a_left[i] != a_right[i]) return i;
		}
		return count;
	}

	// Both writers on one tree; a_output is reused, as callers keep it across files.
	void Check(const OutNode& a_root, std::vector<std::uint8_t>& a_output, std::string_view a_what)
	{
		std::vector<std::uint8_t> expected;
		std::string expectedError;
		const bool expectedOk = detail::WriteNifWithNifly(a_root, expected, &expectedError);

		std::string error;
		const bool ok = WriteNif(a_root, a_output, &error);
		if (ok != expectedOk || error != expectedError || (ok && a_output != expected)) {
			std::cerr << a_what << ": WriteNif " << (ok ? "wrote " : "failed: ") << (ok ? std::to_string(a_output.size()) + " bytes" : error)
					  << ", nifly " << (expectedOk ? "wrote " : "failed: ") << (expectedOk ? std::to_string(expected.size()) + " bytes" : expectedError);
			if (ok && expectedOk) std::cerr << ", first difference at byte " << FirstDifference(a_output, expected);
			std::cerr << "\n";
			assert(false);
		}

		// WriteNif sizes an empty vector for the file it planned and trims it to what it wrote; growing an empty vector
		// allocates exactly what was asked for, so the capacity is the planned size.
		if (ok) {
			std::vector<std::uint8_t> fresh;
			assert(WriteNif(a_root, fresh));
			if (fresh.capacity() != fresh.size()) {
				std::cerr << a_what << ": planned " << fresh.capacity() << " bytes, wrote " << fresh.size() << "\n";
				assert(false);
			}
		}
	}

	OutShape Grid(std::size_t a_vertices, std::string a_name)
	{
		OutShape shape;
		shape.name = std::move(a_name);
		const auto side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(a_vertices))));
		for (std::size_t i = 0; i < a_vertices; ++i) {
			const float x = static_cast<float>(i % side);
			const float y = static_cast<float>(i / side);
			shape.positions.push_back({ x * 16.0f, y * 16.0f, std::sin(x * 0.1f) * 40.0f + std::cos(y * 0.07f) * 25.0f });
			shape.normals.push_back(Normalize({ -std::cos(x * 0.1f) * 0.4f, std::sin(y * 0.07f) * 0.175f, 1.0f }));
			shape.uvs.push_back({ x / static_cast<float>(side), y / static_cast<float>(side) });
		}
		for (std::size_t y = 0; y + 1 < side; ++y) {
			for (std::size_t x = 0; x + 1 < side; ++x) {
				const auto a = y * side + x;
				const auto b = a + 1;
				const auto c = a + side;
				const auto d = c + 1;
				if (d >= a_vertices) continue;
				for (const auto index : { a, c, b, b, c, d }) shape.indices.push_back(static_cast<std::uint16_t>(index));
			}
		}
		return shape;
	}

	void Time()
	{
		OutNode root{ .type = NodeType::BSFadeNode, .name = "obj" };
		OutNode bound{ .type = NodeType::BSMultiBoundNode, .multiBound = AxisAlignedBox{ { 0, 0, 0 }, { 1, 1, 1 } } };
		for (int i = 0; i < 20; ++i) {
			auto shape = Grid(60000, "shape" + std::to_string(i));
			shape.subIndex = true;
			shape.segments = { { 0, static_cast<std::uint32_t>(shape.indices.size() / 3) } };
			shape.colors.assign(shape.positions.size(), Vec4{ 1.0f, 0.5f, 0.25f, 1.0f });
			shape.shader.textures[0] = "textures\\lod\\a.dds";
			bound.shapes.push_back(std::move(shape));
		}
		root.children.push_back(std::move(bound));

		std::vector<std::uint8_t> bytes;
		const auto time = [&](auto&& a_write) {
			double best = 1e30;
			for (int run = 0; run < 5; ++run) {
				const auto start = std::chrono::steady_clock::now();
				const bool ok = a_write();
				assert(ok);
				best = (std::min)(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			}
			return best;
		};
		const auto reference = time([&] { return detail::WriteNifWithNifly(root, bytes); });
		const auto direct = time([&] { return WriteNif(root, bytes); });
		std::cout << "20 shapes x 60000 vertices (" << bytes.size() << " bytes), best of 5: nifly " << reference << " ms, WriteNif " << direct
				  << " ms\n";
	}
}

int main(int argc, char** argv)
{
	std::vector<std::uint8_t> output;

	// Random trees.
	constexpr int kTrees = 400;
	for (int seed = 0; seed < kTrees; ++seed) {
		Rng rng(static_cast<Rng::result_type>(seed));
		const auto root = RandomNode(rng, 0);
		Check(root, output, "seed " + std::to_string(seed));
	}

	// A root of each type, alone, and the smallest shapes.
	for (const auto type : { NodeType::NiNode, NodeType::BSFadeNode, NodeType::BSMultiBoundNode }) {
		Check(OutNode{ .type = type }, output, "lone root");
		OutNode root{ .type = type, .name = "root", .multiBound = AxisAlignedBox{} };
		root.shapes.emplace_back();
		root.shapes.back().shader.kind = ShaderKind::None;
		root.shapes.emplace_back();
		root.shapes.back().subIndex = true;
		Check(root, output, "empty shapes");
	}

	// The most vertices a shape holds, with more triangles than SSE's 16-bit count keeps; then one vertex too many,
	// which both writers refuse with the same message.
	{
		Rng rng(12345);
		OutNode root{ .type = NodeType::BSMultiBoundNode, .name = "full" };
		auto shape = RandomShape(rng, 65535);
		shape.normals.resize(65535);
		for (auto& n : shape.normals) n = UnitVector(rng);
		shape.uvs.resize(65535);
		for (auto& uv : shape.uvs) uv = { Uniform(rng, 0.0f, 1.0f), Uniform(rng, 0.0f, 1.0f) };
		shape.indices.clear();
		for (int i = 0; i < 70000 * 3; ++i) shape.indices.push_back(static_cast<std::uint16_t>(Int(rng, 0, 65534)));
		root.shapes.push_back(shape);
		Check(root, output, "65535 vertices");

		OutNode child{ .name = "child" };
		child.shapes.push_back(shape);
		child.shapes.back().name = "too many";
		child.shapes.back().positions.push_back({});
		root.children.push_back(std::move(child));
		const auto before = output;
		Check(root, output, "65536 vertices");
		std::string error;
		assert(!WriteNif(root, output, &error));
		assert(error == "shape 'too many' has more than 65535 vertices");
		// A failed write leaves the output as it was, as nifly's did.
		assert(output == before);
	}

	// A tree freed after it's written, as a caller's tree on the stack is, then another tree on the same thread: nothing
	// WriteNif keeps between calls may still point into the first. Each write ends a different way: written, refused for
	// a large shape.
	for (const bool refused : { false, true }) {
		std::vector<std::uint8_t> bytes;
		{
			auto tree = std::make_unique<OutNode>();
			tree->name = std::string(64, 'r');
			for (int i = 0; i < 200; ++i) {
				OutShape shape;
				shape.name = "a shape name long enough to be on the heap " + std::to_string(i);
				shape.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
				shape.indices = { 0, 1, 2 };
				tree->shapes.push_back(std::move(shape));
			}
			if (refused) tree->shapes.back().positions.resize(65536);
			assert(WriteNif(*tree, bytes) != refused);
		}
		// Reuse the freed memory with other contents.
		std::vector<std::string> churn;
		for (int i = 0; i < 400; ++i) churn.emplace_back(64, static_cast<char>('A' + i % 26));

		Rng rng(4242);
		const auto next = RandomNode(rng, 1);
		Check(next, output, refused ? "after a refused, freed tree" : "after a freed tree");
	}

	// Writing into the same vector twice, and after a larger file, gives the same bytes.
	{
		Rng rng(777);
		const auto small = RandomNode(rng, 2);
		const auto large = RandomNode(rng, 0);
		std::vector<std::uint8_t> fresh;
		assert(WriteNif(small, fresh));
		std::vector<std::uint8_t> reused;
		assert(WriteNif(small, reused));
		assert(reused == fresh);
		assert(WriteNif(small, reused));
		assert(reused == fresh);
		assert(WriteNif(large, reused));
		assert(WriteNif(small, reused));
		assert(reused == fresh);
	}

	if (argc > 1 && std::string_view(argv[1]) == "--time") Time();

	std::cout << "tesworld nif writer tests passed\n";
	return 0;
}
