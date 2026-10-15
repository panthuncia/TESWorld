#include "tesworld/Render.h"
#include "tesworld/Profiling.h"


#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace tesworld::render
{
	namespace
	{
		struct Vertex
		{
			Vec3 world;
			float sx{ 0.0f };
			float sy{ 0.0f };
			float depth{ 0.0f };
		};

		Vec3 Lerp3(Vec3 a, Vec3 b, Vec3 c, float w0, float w1, float w2) { return a * w0 + b * w1 + c * w2; }

		Vec4 Multiply(Vec4 a, Vec4 b) { return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w }; }

		// A tangent-space normal map's texel as a normal in the shape's frame: Skyrim's maps are green-up (OpenGL).
		Vec3 FromTangentSpace(Vec4 a_texel, Vec3 a_normal, Vec3 a_tangent, Vec3 a_bitangent)
		{
			const Vec3 t{ a_texel.x * 2.0f - 1.0f, a_texel.y * 2.0f - 1.0f, a_texel.z * 2.0f - 1.0f };
			return Normalize(a_tangent * t.x + a_bitangent * t.y + a_normal * t.z, a_normal);
		}
	}

	bool Draws(const nif::Shape& a_shape, bool a_skinned)
	{
		return a_shape.shader.kind == nif::ShaderKind::Lighting && (a_skinned || !a_shape.skinned) && !(a_shape.shader.alphaBlend && !a_shape.shader.alphaTest) &&
		       !a_shape.positions.empty() && a_shape.indices.size() >= 3;
	}

	std::pair<Vec3, Vec3> Bounds(std::span<const DrawItem> a_items, bool a_skinned)
	{
		constexpr float big = std::numeric_limits<float>::max();
		Vec3 min{ big, big, big }, max{ -big, -big, -big };
		for (const auto& item : a_items) {
			if (!item.model) continue;
			for (const auto& shape : item.model->shapes) {
				if (!Draws(shape, a_skinned)) continue;
				for (const auto& p : shape.positions) {
					const auto w = item.transform.Apply(p);
					min = { (std::min)(min.x, w.x), (std::min)(min.y, w.y), (std::min)(min.z, w.z) };
					max = { (std::max)(max.x, w.x), (std::max)(max.y, w.y), (std::max)(max.z, w.z) };
				}
			}
		}
		return { min, max };
	}

	Frame CpuRenderer::Render(std::span<const DrawItem> a_items, const Camera& a_camera, const RenderOptions& a_options, const TextureSource& a_textures)
	{
		TW_ZONE("CpuRenderer::Render");
		const std::uint32_t ss = (std::max)(1u, a_options.supersample);
		const std::uint32_t width = a_options.width * ss, height = a_options.height * ss;
		const std::size_t pixels = static_cast<std::size_t>(width) * height;
		std::vector<float> depth(pixels, std::numeric_limits<float>::infinity());
		std::vector<Vec4> color(pixels);
		std::vector<Vec3> normal(pixels);
		std::vector<std::uint8_t> covered(pixels, 0);
		const auto toCamera = a_camera.forward * -1.0f;
		const auto& light = a_options.lighting;

		const auto project = [&](Vec3 a_world) {
			const auto d = a_world - a_camera.center;
			return Vertex{
				.world = a_world,
				.sx = (Dot(d, a_camera.right) / a_camera.width + 0.5f) * static_cast<float>(width),
				.sy = (0.5f - Dot(d, a_camera.up) / a_camera.height) * static_cast<float>(height),
				.depth = Dot(d, a_camera.forward),
			};
		};

		for (const auto& item : a_items) {
			if (!item.model) continue;
			for (const auto& shape : item.model->shapes) {
				if (!Draws(shape, a_options.skinned)) continue;
				const auto& shader = shape.shader;
				const auto* diffuse = shader.textures[0].empty() ? nullptr : a_textures(shader.textures[0]);
				const auto* normalMap = shader.textures[1].empty() ? nullptr : a_textures(shader.textures[1]);
				const bool modelSpace = shader.ModelSpaceNormals();
				const bool hasUv = shape.uvs.size() == shape.positions.size();
				const bool hasNormals = shape.normals.size() == shape.positions.size();
				const bool hasTangents = hasNormals && shape.tangents.size() == shape.positions.size() && shape.bitangents.size() == shape.positions.size();
				const bool vertexColors = shader.VertexColors() && shape.colors.size() == shape.positions.size();
				const float threshold = shader.alphaTest ? static_cast<float>(shader.alphaThreshold) / 255.0f : 0.5f;
				const bool cutout = shader.alphaTest || shader.alphaBlend;
				// A tree's vertex alpha is how much the wind moves it, not how opaque it is; grass's too.
				const bool vertexAlpha = shader.VertexAlpha() && !shader.TreeAnim() && !a_options.windAlpha;

				std::vector<Vertex> projected(shape.positions.size());
				for (std::size_t v = 0; v < shape.positions.size(); ++v) projected[v] = project(item.transform.Apply(shape.positions[v]));

				for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3) {
					const std::uint32_t i0 = shape.indices[t], i1 = shape.indices[t + 1], i2 = shape.indices[t + 2];
					if (i0 >= projected.size() || i1 >= projected.size() || i2 >= projected.size()) continue;
					const auto& a = projected[i0];
					const auto& b = projected[i1];
					const auto& c = projected[i2];
					const float area = (b.sx - a.sx) * (c.sy - a.sy) - (c.sx - a.sx) * (b.sy - a.sy);
					if (std::abs(area) < 1e-9f) continue;
					const Vec3 faceNormal = Normalize(Cross(b.world - a.world, c.world - a.world), toCamera);

					// The texture level matching the triangle's texels per sample.
					float lod = 0.0f;
					if (diffuse && hasUv) {
						const auto& ua = shape.uvs[i0];
						const auto& ub = shape.uvs[i1];
						const auto& uc = shape.uvs[i2];
						const float uvArea = std::abs((ub.x - ua.x) * (uc.y - ua.y) - (uc.x - ua.x) * (ub.y - ua.y)) * static_cast<float>(diffuse->Width()) *
						                     static_cast<float>(diffuse->Height());
						lod = (std::max)(0.0f, 0.5f * std::log2((std::max)(uvArea / std::abs(area), 1e-12f)));
					}

					const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ a.sx, b.sx, c.sx }))));
					const int x1 = (std::min)(static_cast<int>(width) - 1, static_cast<int>(std::ceil((std::max)({ a.sx, b.sx, c.sx }))));
					const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ a.sy, b.sy, c.sy }))));
					const int y1 = (std::min)(static_cast<int>(height) - 1, static_cast<int>(std::ceil((std::max)({ a.sy, b.sy, c.sy }))));
					for (int y = y0; y <= y1; ++y) {
						for (int x = x0; x <= x1; ++x) {
							const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
							const float w0 = ((b.sx - px) * (c.sy - py) - (c.sx - px) * (b.sy - py)) / area;
							const float w1 = ((c.sx - px) * (a.sy - py) - (a.sx - px) * (c.sy - py)) / area;
							const float w2 = 1.0f - w0 - w1;
							if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
							const float z = a.depth * w0 + b.depth * w1 + c.depth * w2;
							const auto index = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
							if (z >= depth[index]) continue;

							Vec4 albedo{ 0.6f, 0.6f, 0.6f, shader.alpha };
							Vec2 uv{};
							if (hasUv) {
								const auto& ua = shape.uvs[i0];
								const auto& ub = shape.uvs[i1];
								const auto& uc = shape.uvs[i2];
								uv = { (ua.x * w0 + ub.x * w1 + uc.x * w2) * shader.uvScale.x + shader.uvOffset.x,
									(ua.y * w0 + ub.y * w1 + uc.y * w2) * shader.uvScale.y + shader.uvOffset.y };
								if (diffuse) {
									const auto texel = texture::SampleLevel(*diffuse, uv, lod, texture::Address::Wrap);
									// A cutout's alpha is tested at the full size: smaller levels average it away, and
									// leaves thin out. Supersampling smooths the edges instead.
									const float alpha = cutout ? texture::SampleLevel(*diffuse, uv, 0.0f, texture::Address::Wrap).w : texel.w;
									albedo = { texel.x, texel.y, texel.z, alpha * shader.alpha };
								}
							}
							if (vertexColors) {
								const auto& ca = shape.colors[i0];
								const auto& cb = shape.colors[i1];
								const auto& cc = shape.colors[i2];
								const Vec4 vc{ ca.x * w0 + cb.x * w1 + cc.x * w2, ca.y * w0 + cb.y * w1 + cc.y * w2, ca.z * w0 + cb.z * w1 + cc.z * w2,
									vertexAlpha ? ca.w * w0 + cb.w * w1 + cc.w * w2 : 1.0f };
								albedo = Multiply(albedo, vc);
							}
							if (cutout && albedo.w < threshold) continue;

							// The surface's normal, turned to face the camera: both faces are drawn.
							Vec3 n = faceNormal;
							if (hasNormals) {
								n = Normalize(Lerp3(shape.normals[i0], shape.normals[i1], shape.normals[i2], w0, w1, w2), faceNormal);
								if (normalMap && hasUv) {
									const auto texel = texture::SampleLevel(*normalMap, uv, lod, texture::Address::Wrap);
									if (modelSpace) {
										n = Normalize({ texel.x * 2.0f - 1.0f, texel.z * 2.0f - 1.0f, texel.y * 2.0f - 1.0f }, n);
									} else if (hasTangents) {
										n = FromTangentSpace(texel, n, Normalize(Lerp3(shape.tangents[i0], shape.tangents[i1], shape.tangents[i2], w0, w1, w2)),
											Normalize(Lerp3(shape.bitangents[i0], shape.bitangents[i1], shape.bitangents[i2], w0, w1, w2)));
									}
								}
								n = Normalize(item.transform.RotateVector(n), faceNormal);
							}
							if (Dot(n, toCamera) < 0.0f) n = n * -1.0f;

							const float shade = light.ambient + light.direct * (std::max)(0.0f, Dot(n, light.toLight));
							depth[index] = z;
							color[index] = { albedo.x * shade, albedo.y * shade, albedo.z * shade, 1.0f };
							normal[index] = n;
							covered[index] = 1;
						}
					}
				}
			}
		}

		// Box-filtered down: each pixel's colour the mean of its covered samples, its alpha how many are.
		Frame frame{ .color = texture::Image(a_options.width, a_options.height), .normal = texture::Image(a_options.width, a_options.height) };
		const float samples = static_cast<float>(ss * ss);
		for (std::uint32_t y = 0; y < a_options.height; ++y) {
			for (std::uint32_t x = 0; x < a_options.width; ++x) {
				Vec4 sum{};
				Vec3 normalSum{};
				int count = 0;
				for (std::uint32_t sy = 0; sy < ss; ++sy) {
					for (std::uint32_t sx = 0; sx < ss; ++sx) {
						const auto index = static_cast<std::size_t>(y * ss + sy) * width + (x * ss + sx);
						if (!covered[index]) continue;
						sum = { sum.x + color[index].x, sum.y + color[index].y, sum.z + color[index].z, 0.0f };
						normalSum = normalSum + normal[index];
						++count;
					}
				}
				if (count == 0) continue;
				const float inverse = 1.0f / static_cast<float>(count);
				const float alpha = static_cast<float>(count) / samples;
				frame.color.SetTexel(x, y, { (std::min)(1.0f, sum.x * inverse), (std::min)(1.0f, sum.y * inverse), (std::min)(1.0f, sum.z * inverse), alpha });
				const auto n = Normalize(normalSum, toCamera);
				const Vec3 view{ Dot(n, a_camera.right), Dot(n, a_camera.up), Dot(n, toCamera) };
				frame.normal.SetTexel(x, y, { view.x * 0.5f + 0.5f, view.y * 0.5f + 0.5f, view.z * 0.5f + 0.5f, alpha });
			}
		}
		return frame;
	}
}