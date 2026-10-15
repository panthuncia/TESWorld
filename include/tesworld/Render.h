#pragma once

#include "tesworld/Math.h"
#include "tesworld/Nif.h"
#include "tesworld/Texture.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace tesworld::render
{
	// An orthographic view: what lies in a box a_width by a_height across, centred on center, seen along forward;
	// right and up run across the image. The three are unit vectors at right angles.
	struct Camera
	{
		Vec3 center{};
		Vec3 right{ 1.0f, 0.0f, 0.0f };
		Vec3 up{ 0.0f, 0.0f, 1.0f };
		Vec3 forward{ 0.0f, 1.0f, 0.0f };
		float width{ 1.0f };
		float height{ 1.0f };
	};

	// One directional light and an ambient term: colour = albedo * (ambient + direct * max(0, n . toLight)).
	struct Lighting
	{
		Vec3 toLight{ Normalize({ -0.35f, -0.6f, 0.72f }) };
		float direct{ 0.55f };
		float ambient{ 0.6f };
	};

	struct DrawItem
	{
		const nif::Model* model{ nullptr };
		Transform transform{};
	};

	// A texture by its game path (with or without "textures\"); null when it can't be read. Called from one thread.
	using TextureSource = std::function<const texture::MipChain*(const std::string&)>;

	struct RenderOptions
	{
		std::uint32_t width{ 256 };
		std::uint32_t height{ 256 };
		// Samples per pixel along each side: edges and alpha-tested leaves come out smooth.
		std::uint32_t supersample{ 4 };
		Lighting lighting{};
		// Skinned shapes drawn as they're stored (their bind pose): a tree's branches, skinned to its wind bones.
		bool skinned{ false };
		// Vertex alpha is how much the wind moves the model, not how opaque it is, whatever its shader says: grass's.
		// (A tree-animated shape's always is.)
		bool windAlpha{ false };
	};

	// A rendered frame. Colour is lit and unpremultiplied, alpha the share of the pixel covered. Normals face the camera,
	// in its frame (x right, y up, z toward the viewer), encoded as a normal map's (n * 0.5 + 0.5), alpha as colour's.
	struct Frame
	{
		texture::Image color;
		texture::Image normal;
	};

	// Draws models into an image. A GPU backend can stand behind the same interface.
	class IRenderer
	{
	public:
		virtual ~IRenderer() = default;

		[[nodiscard]] virtual Frame Render(std::span<const DrawItem> a_items, const Camera& a_camera, const RenderOptions& a_options,
			const TextureSource& a_textures) = 0;
	};

	// On the CPU: rasterized at supersample times the size, z-buffered, alpha-tested, both faces of every triangle drawn
	// facing the camera, lit by vertex normals (or model-space and tangent-space normal maps), then box-filtered down.
	class CpuRenderer final : public IRenderer
	{
	public:
		[[nodiscard]] Frame Render(std::span<const DrawItem> a_items, const Camera& a_camera, const RenderOptions& a_options,
			const TextureSource& a_textures) override;
	};

	// A model to render out of the caller's way (ViewRenderer): drawn as it's stored, with the textures its shaders name.
	struct ViewRequest
	{
		std::shared_ptr<const nif::Model> model;
		Camera camera;
		RenderOptions options;
		// By the paths the shapes' shaders name, as they name them; one missing is drawn without, as a TextureSource's null.
		std::map<std::string, std::shared_ptr<const texture::MipChain>> textures;
	};

	struct ViewResult
	{
		Frame frame;
		std::string error;

		[[nodiscard]] bool Ok() const { return error.empty(); }
	};

	// Renders views as CpuRenderer does, out of the caller's way: on the CPU across its cores, or on the GPU
	// (RenderGpu.h), many views to a frame, when there is one. What a GPU renders differs from the CPU's by its
	// rasterization and arithmetic, not by what's drawn.
	class ViewRenderer
	{
	public:
		using Callback = std::function<void(ViewResult)>;

		virtual ~ViewRenderer() = default;

		// Renders, then calls a_done from a TBB task: a renderer may batch requests and finish them in any order.
		virtual void Render(ViewRequest a_request, Callback a_done) = 0;
		// Returns once every request so far has called back.
		virtual void Wait() = 0;
		// "CPU", or the GPU's API and adapter.
		[[nodiscard]] virtual std::string Backend() const = 0;
		// What went wrong that the renderer worked around (a GPU that failed part way), or nothing.
		[[nodiscard]] virtual std::string Problem() const { return {}; }
	};

	// CpuRenderer in a task of its own for each request.
	[[nodiscard]] std::unique_ptr<ViewRenderer> CreateCpuViewRenderer();

	// The box holding the items' drawn shapes, transformed; {max, min} inverted when there are none.
	[[nodiscard]] std::pair<Vec3, Vec3> Bounds(std::span<const DrawItem> a_items, bool a_skinned = false);

	// Whether a shape is drawn: geometry with a lighting shader, not blended glass; skinned only when asked for.
	[[nodiscard]] bool Draws(const nif::Shape& a_shape, bool a_skinned = false);
}