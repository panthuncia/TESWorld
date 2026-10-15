// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Render.h"
#if defined(TESWORLD_TEST_GPU)
#include "tesworld/RenderGpu.h"
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace tesworld;
using namespace tesworld::render;

namespace
{
	bool Near(float a, float b, float tolerance = 0.03f) { return std::abs(a - b) <= tolerance; }

	// A quad across x and z at depth y, facing -y, its UVs over the whole texture.
	nif::Shape Quad(float a_x0, float a_x1, float a_z0, float a_z1, float a_y, const std::string& a_texture)
	{
		nif::Shape shape;
		shape.positions = { { a_x0, a_y, a_z0 }, { a_x1, a_y, a_z0 }, { a_x1, a_y, a_z1 }, { a_x0, a_y, a_z1 } };
		shape.normals = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 } };
		shape.uvs = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
		shape.indices = { 0, 1, 2, 0, 2, 3 };
		shape.shader.kind = nif::ShaderKind::Lighting;
		shape.shader.textures[0] = a_texture;
		return shape;
	}

	texture::MipChain Solid(Vec4 a_left, Vec4 a_right)
	{
		texture::Image image(2, 1);
		image.SetTexel(0, 0, a_left);
		image.SetTexel(1, 0, a_right);
		texture::MipChain chain;
		chain.levels.push_back(image);
		return chain;
	}

	struct Textures
	{
		std::map<std::string, texture::MipChain> chains;

		TextureSource Source()
		{
			return [this](const std::string& a_path) -> const texture::MipChain* {
				const auto it = chains.find(a_path);
				return it == chains.end() ? nullptr : &it->second;
			};
		}
	};

	// Looking along +y at a box 200 wide and 100 high around the origin, into 64 by 32 pixels.
	Camera Front() { return Camera{ .center = {}, .right = { 1, 0, 0 }, .up = { 0, 0, 1 }, .forward = { 0, 1, 0 }, .width = 200, .height = 100 }; }

	void AQuadCoversItsShareAndIsLitFacingTheCamera()
	{
		Textures textures;
		textures.chains["red.dds"] = Solid({ 1, 0, 0, 1 }, { 1, 0, 0, 1 });
		nif::Model model;
		model.shapes.push_back(Quad(-50, 50, -25, 25, 0, "red.dds"));
		const DrawItem item{ .model = &model };
		const RenderOptions options{ .width = 64, .height = 32, .supersample = 4, .lighting = { .toLight = { 0, -1, 0 }, .direct = 0.5f, .ambient = 0.25f } };

		const auto frame = CpuRenderer().Render(std::span(&item, 1), Front(), options, textures.Source());

		// The quad is the middle half across and up: pixels 16..47 by 8..23.
		for (std::uint32_t y = 0; y < 32; ++y) {
			for (std::uint32_t x = 0; x < 64; ++x) {
				const bool inside = x >= 16 && x < 48 && y >= 8 && y < 24;
				assert(Near(frame.color.Texel(x, y).w, inside ? 1.0f : 0.0f));
			}
		}
		// Facing the light: ambient and all of the direct light.
		const auto lit = frame.color.Texel(32, 16);
		assert(Near(lit.x, 0.75f) && Near(lit.y, 0.0f));
		// Its normal faces the camera: z toward the viewer.
		const auto n = frame.normal.Texel(32, 16);
		assert(Near(n.x, 0.5f) && Near(n.y, 0.5f) && Near(n.z, 1.0f));
	}

	void TheNearerSurfaceWinsWhicheverIsDrawnFirst()
	{
		Textures textures;
		textures.chains["red.dds"] = Solid({ 1, 0, 0, 1 }, { 1, 0, 0, 1 });
		textures.chains["blue.dds"] = Solid({ 0, 0, 1, 1 }, { 0, 0, 1, 1 });
		nif::Model model;
		model.shapes.push_back(Quad(-50, 0, -25, 25, -10, "blue.dds")); // nearer, on the left
		model.shapes.push_back(Quad(-50, 50, -25, 25, 0, "red.dds"));
		const DrawItem item{ .model = &model };
		const RenderOptions options{ .width = 64, .height = 32, .supersample = 2 };

		const auto frame = CpuRenderer().Render(std::span(&item, 1), Front(), options, textures.Source());

		assert(frame.color.Texel(20, 16).z > 0.3f && frame.color.Texel(20, 16).x < 0.01f);
		assert(frame.color.Texel(44, 16).x > 0.3f && frame.color.Texel(44, 16).z < 0.01f);
	}

	void AlphaTestedTexelsAreLeftOutAndEdgesAreHalfCovered()
	{
		Textures textures;
		textures.chains["leaf.dds"] = Solid({ 0, 1, 0, 0 }, { 0, 1, 0, 1 });
		nif::Model model;
		auto quad = Quad(-50, 50, -25, 25, 0, "leaf.dds");
		quad.shader.alphaTest = true;
		quad.shader.alphaThreshold = 128;
		model.shapes.push_back(quad);
		const DrawItem item{ .model = &model };

		// One pixel per texel half across: the left texel is cut out, the right kept.
		const auto frame = CpuRenderer().Render(std::span(&item, 1), Front(), RenderOptions{ .width = 64, .height = 32, .supersample = 4 }, textures.Source());

		assert(Near(frame.color.Texel(20, 16).w, 0.0f));
		assert(Near(frame.color.Texel(44, 16).w, 1.0f));

		// A quad edge through the middle of a pixel covers half of it.
		nif::Model half;
		half.shapes.push_back(Quad(-50 + 200.0f / 64 / 2, 50, -25, 25, 0, "missing.dds"));
		const DrawItem halfItem{ .model = &half };
		const auto edge = CpuRenderer().Render(std::span(&halfItem, 1), Front(), RenderOptions{ .width = 64, .height = 32, .supersample = 4 }, textures.Source());
		assert(Near(edge.color.Texel(16, 16).w, 0.5f, 0.01f));
	}

	void GrassVertexAlphaIsWindNotOpacity()
	{
		// Grass's shader takes vertex alpha, but its alpha is how much the wind moves each vertex: 0 at the roots.
		Textures textures;
		textures.chains["green.dds"] = Solid({ 0, 1, 0, 1 }, { 0, 1, 0, 1 });
		nif::Model model;
		auto blade = Quad(-50, 50, -25, 25, 0, "green.dds");
		blade.shader.flags1 |= 1u << 3;
		blade.shader.flags2 |= 0x20u;
		blade.colors.assign(4, Vec4{ 1, 1, 1, 0 });
		blade.shader.alphaTest = true;
		blade.shader.alphaThreshold = 128;
		model.shapes.push_back(blade);
		const DrawItem item{ .model = &model };
		RenderOptions options{ .width = 64, .height = 32, .supersample = 1 };

		assert(Near(CpuRenderer().Render(std::span(&item, 1), Front(), options, textures.Source()).color.Texel(32, 16).w, 0.0f));
		options.windAlpha = true;
		assert(Near(CpuRenderer().Render(std::span(&item, 1), Front(), options, textures.Source()).color.Texel(32, 16).w, 1.0f));
	}

	void TransformsMoveTheModelAndBoundsFollow()
	{
		nif::Model model;
		model.shapes.push_back(Quad(-1, 1, -1, 1, 0, ""));
		Transform transform;
		transform.translation = { 10, 0, 5 };
		transform.scale = 2;
		const DrawItem item{ .model = &model, .transform = transform };

		const auto [min, max] = Bounds(std::span(&item, 1));

		assert(Near(min.x, 8) && Near(max.x, 12) && Near(min.z, 3) && Near(max.z, 7));
		nif::Model empty;
		const DrawItem none{ .model = &empty };
		const auto [noMin, noMax] = Bounds(std::span(&none, 1));
		assert(noMin.x > noMax.x);
	}

	// A tree as billboards see one: a trunk of vertex-coloured quads, and leaf cards cut out of a round texture with a
	// tangent-space normal map, at many angles.
	std::map<std::string, std::shared_ptr<const texture::MipChain>> TreeTextures()
	{
		texture::Image bark(64, 64), leaf(64, 64), bump(64, 64);
		for (std::uint32_t y = 0; y < 64; ++y) {
			for (std::uint32_t x = 0; x < 64; ++x) {
				const float u = (static_cast<float>(x) + 0.5f) / 64.0f, v = (static_cast<float>(y) + 0.5f) / 64.0f;
				bark.SetTexel(x, y, { 0.4f + 0.2f * std::sin(u * 40.0f), 0.3f, 0.2f + 0.1f * v, 1.0f });
				const float r = std::hypot(u - 0.5f, v - 0.5f);
				leaf.SetTexel(x, y, { 0.2f + 0.3f * u, 0.5f + 0.4f * v, 0.15f, r < 0.45f ? 1.0f : 0.0f });
				bump.SetTexel(x, y, { 0.5f + 0.3f * std::sin(u * 20.0f), 0.5f + 0.3f * std::cos(v * 20.0f), 0.9f, 1.0f });
			}
		}
		std::map<std::string, std::shared_ptr<const texture::MipChain>> textures;
		textures["bark.dds"] = std::make_shared<const texture::MipChain>(texture::BuildMips(bark));
		textures["leaf.dds"] = std::make_shared<const texture::MipChain>(texture::BuildMips(leaf, 0.5f));
		textures["leaf_n.dds"] = std::make_shared<const texture::MipChain>(texture::BuildMips(bump));
		return textures;
	}

	std::shared_ptr<const nif::Model> Tree()
	{
		auto model = std::make_shared<nif::Model>();
		for (int side = 0; side < 4; ++side) {
			auto trunk = Quad(-8, 8, 0, 300, 0, "bark.dds");
			const float angle = static_cast<float>(side) * 1.5708f;
			for (auto& p : trunk.positions) p = { p.x * std::cos(angle) - (p.y - 8) * std::sin(angle), p.x * std::sin(angle) + (p.y - 8) * std::cos(angle), p.z };
			for (auto& n : trunk.normals) n = { std::sin(angle), -std::cos(angle), 0 };
			trunk.colors = { { 1, 1, 1, 1 }, { 0.8f, 0.8f, 0.8f, 1 }, { 0.6f, 0.7f, 0.6f, 1 }, { 1, 0.9f, 0.9f, 1 } };
			trunk.shader.flags2 |= 1u << 5;
			model->shapes.push_back(trunk);
		}
		std::uint32_t seed = 7;
		const auto random = [&] {
			seed = seed * 1664525u + 1013904223u;
			return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
		};
		for (int i = 0; i < 60; ++i) {
			auto card = Quad(-40, 40, -40, 40, 0, "leaf.dds");
			card.shader.textures[1] = "leaf_n.dds";
			card.shader.alphaTest = true;
			card.shader.alphaThreshold = 128;
			const float yaw = random() * 6.2832f, pitch = (random() - 0.5f) * 1.2f;
			const Vec3 at{ (random() - 0.5f) * 160.0f, (random() - 0.5f) * 160.0f, 220.0f + random() * 160.0f };
			const auto turn = [&](Vec3 a_v) {
				const Vec3 tilted{ a_v.x, a_v.y * std::cos(pitch) - a_v.z * std::sin(pitch), a_v.y * std::sin(pitch) + a_v.z * std::cos(pitch) };
				return Vec3{ tilted.x * std::cos(yaw) - tilted.y * std::sin(yaw), tilted.x * std::sin(yaw) + tilted.y * std::cos(yaw), tilted.z };
			};
			for (auto& p : card.positions) p = turn(p) + at;
			for (auto& n : card.normals) n = turn(n);
			card.tangents.assign(4, turn({ 1, 0, 0 }));
			card.bitangents.assign(4, turn({ 0, 0, 1 }));
			model->shapes.push_back(card);
		}
		return model;
	}

	Camera Fitted(int a_side, float a_x0, float a_x1, float a_z0, float a_z1)
	{
		const Vec3 forward = a_side == 0 ? Vec3{ 0, 1, 0 } : Vec3{ 1, 0, 0 };
		const Vec3 right = a_side == 0 ? Vec3{ 1, 0, 0 } : Vec3{ 0, -1, 0 };
		return Camera{ .center = right * ((a_x0 + a_x1) * 0.5f) + Vec3{ 0, 0, (a_z0 + a_z1) * 0.5f }, .right = right, .up = { 0, 0, 1 }, .forward = forward,
			.width = a_x1 - a_x0, .height = a_z1 - a_z0 };
	}

	// Views as a billboard's: probes and fitted views of either side, of several sizes; and grass, whose vertex alpha is
	// wind.
	std::vector<ViewRequest> TreeViews()
	{
		const auto model = Tree();
		const auto textures = TreeTextures();
		std::vector<ViewRequest> views;
		for (std::uint32_t i = 0; i < 24; ++i) {
			const int side = static_cast<int>(i % 2);
			const std::uint32_t high = i % 3 == 0 ? 256u : 32u << (i % 4);
			const std::uint32_t wide = i % 3 == 0 ? 256u : (std::max)(4u, high * (2 + i % 3) / 4 / 4 * 4);
			const float grow = static_cast<float>(i % 5) * 4.0f;
			views.push_back(ViewRequest{ .model = model, .camera = Fitted(side, -130 - grow, 130 + grow, -5, 420 + grow),
				.options = RenderOptions{ .width = wide, .height = high, .supersample = 4, .skinned = true }, .textures = textures });
		}
		auto grass = std::make_shared<nif::Model>();
		auto blade = Quad(-20, 20, 0, 60, 0, "leaf.dds");
		blade.colors = { { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0.2f }, { 1, 1, 1, 0.2f } };
		blade.shader.flags1 |= 1u << 3;
		blade.shader.flags2 |= 1u << 5;
		blade.shader.alphaTest = true;
		grass->shapes.push_back(blade);
		views.push_back(ViewRequest{ .model = grass, .camera = Fitted(0, -25, 25, -2, 62),
			.options = RenderOptions{ .width = 64, .height = 64, .supersample = 4, .windAlpha = true }, .textures = textures });
		return views;
	}

	std::vector<ViewResult> RenderAll(ViewRenderer& a_renderer, const std::vector<ViewRequest>& a_views)
	{
		std::mutex mutex;
		std::vector<ViewResult> results(a_views.size());
		for (std::size_t i = 0; i < a_views.size(); ++i) {
			a_renderer.Render(a_views[i], [&, i](ViewResult a_result) {
				std::scoped_lock lock(mutex);
				results[i] = std::move(a_result);
			});
		}
		a_renderer.Wait();
		return results;
	}

	void TheCpuViewRendererIsTheCpuRenderer()
	{
		const auto views = TreeViews();
		const auto renderer = CreateCpuViewRenderer();
		const auto results = RenderAll(*renderer, views);
		for (std::size_t i = 0; i < views.size(); ++i) {
			Textures textures;
			for (const auto& [path, chain] : views[i].textures) textures.chains[path] = *chain;
			const DrawItem item{ .model = views[i].model.get() };
			const auto frame = CpuRenderer().Render(std::span(&item, 1), views[i].camera, views[i].options, textures.Source());
			assert(results[i].Ok() && results[i].frame.color.rgba == frame.color.rgba && results[i].frame.normal.rgba == frame.normal.rgba);
		}
	}

#if defined(TESWORLD_TEST_GPU)
	// Peak signal to noise of one image against another, in dB (100 when they're the same).
	double Psnr(const texture::Image& a_image, const texture::Image& a_reference)
	{
		assert(a_image.rgba.size() == a_reference.rgba.size());
		double error = 0.0;
		for (std::size_t i = 0; i < a_image.rgba.size(); ++i) {
			const double d = static_cast<double>(a_image.rgba[i]) - static_cast<double>(a_reference.rgba[i]);
			error += d * d;
		}
		if (error == 0.0) return 100.0;
		return 10.0 * std::log10(255.0 * 255.0 / (error / static_cast<double>(a_image.rgba.size())));
	}

	void SetFailAfter(const char* a_frames)
	{
#if defined(_WIN32)
		_putenv_s("TESWORLD_GPU_FAIL_AFTER", a_frames);
#else
		setenv("TESWORLD_GPU_FAIL_AFTER", a_frames, 1);
#endif
	}

	// On the GPU, as the CPU renders them: coverage, colour and normals within rounding and rasterization at the edges;
	// the same however they're batched.
	void GpuViewsMatchTheCpus()
	{
		try {
			gpu::Context context;
			std::cout << "GPU: " << context.Describe() << '\n';
			const auto gpu = CreateGpuViewRenderer(context);
			const auto cpu = CreateCpuViewRenderer();
			const auto views = TreeViews();
			const auto onGpu = RenderAll(*gpu, views);
			const auto onCpu = RenderAll(*cpu, views);
			double worstColor = 100.0, worstNormal = 100.0;
			for (std::size_t i = 0; i < views.size(); ++i) {
				assert(onGpu[i].Ok() && onCpu[i].Ok());
				worstColor = (std::min)(worstColor, Psnr(onGpu[i].frame.color, onCpu[i].frame.color));
				worstNormal = (std::min)(worstNormal, Psnr(onGpu[i].frame.normal, onCpu[i].frame.normal));
			}
			std::cout << "GPU views against the CPU's: colour " << worstColor << " dB, normals " << worstNormal << " dB at worst\n";
			assert(worstColor > 30.0 && worstNormal > 30.0);

			// One at a time, each in a frame of its own: the same.
			for (std::size_t i = 0; i < views.size(); i += 5) {
				const auto alone = RenderAll(*gpu, { views[i] });
				assert(alone[0].frame.color.rgba == onGpu[i].frame.color.rgba && alone[0].frame.normal.rgba == onGpu[i].frame.normal.rgba);
			}

			// Too large for a frame: the CPU's.
			auto large = views[1];
			large.options.width = 1100;
			const auto big = RenderAll(*gpu, { large });
			assert(big[0].Ok() && big[0].frame.color.rgba == RenderAll(*cpu, { large })[0].frame.color.rgba);
			assert(gpu->Problem().empty());
		} catch (const gpu::UnsupportedError& e) {
			std::cout << "no GPU, its tests skipped: " << e.what() << '\n';
		}
	}

	// A GPU that fails part way: what it owed is rendered on the CPU, and said.
	void AFailingGpuHandsItsViewsToTheCpu()
	{
		SetFailAfter("1");
		try {
			gpu::Context context;
			const auto gpu = CreateGpuViewRenderer(context);
			const auto cpu = CreateCpuViewRenderer();
			auto views = TreeViews();
			for (std::size_t i = 0, n = views.size(); i < n; ++i) views.push_back(views[i]);
			const auto onGpu = RenderAll(*gpu, views);
			const auto onCpu = RenderAll(*cpu, views);
			for (std::size_t i = 0; i < views.size(); ++i) assert(onGpu[i].Ok() && Psnr(onGpu[i].frame.color, onCpu[i].frame.color) > 30.0);
			std::cout << "after failing: " << gpu->Problem() << '\n';
			assert(!gpu->Problem().empty());
		} catch (const gpu::UnsupportedError& e) {
			std::cout << "no GPU, its tests skipped: " << e.what() << '\n';
		}
		SetFailAfter("");
	}
#endif
}

int main()
{
	AQuadCoversItsShareAndIsLitFacingTheCamera();
	TheNearerSurfaceWinsWhicheverIsDrawnFirst();
	AlphaTestedTexelsAreLeftOutAndEdgesAreHalfCovered();
	GrassVertexAlphaIsWindNotOpacity();
	TransformsMoveTheModelAndBoundsFollow();
	TheCpuViewRendererIsTheCpuRenderer();
#if defined(TESWORLD_TEST_GPU)
	GpuViewsMatchTheCpus();
	AFailingGpuHandsItsViewsToTheCpu();
#endif
	std::cout << "render tests passed\n";
	return 0;
}