// tesworld-render <out.png> <height> <yaw> <pitch> <data folder>... -- <mesh>...
// Renders meshes with tesworld::render's CPU renderer, seen from a direction (yaw from +Y toward +X, pitch down from
// level, in degrees), fitted to them: a look at LOD meshes and billboards as they'd stand. Game paths are looked up in the
// data folders (later ones win), including every BSA directly in them.

#include "tesworld/Nif.h"
#include "tesworld/Platform.h"
#include "tesworld/Render.h"
#include "tesworld/Texture.h"
#include "tesworld/Vfs.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>

#include <zlib.h>

using namespace tesworld;

namespace
{
	void Chunk(std::ofstream& a_out, const char* a_type, const std::vector<std::uint8_t>& a_data)
	{
		const auto length = static_cast<std::uint32_t>(a_data.size());
		const std::uint8_t lengthBytes[4] = { static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length) };
		a_out.write(reinterpret_cast<const char*>(lengthBytes), 4);
		a_out.write(a_type, 4);
		if (!a_data.empty()) a_out.write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
		auto crc = crc32(0, reinterpret_cast<const Bytef*>(a_type), 4);
		if (!a_data.empty()) crc = crc32(crc, a_data.data(), static_cast<uInt>(a_data.size()));
		const std::uint8_t crcBytes[4] = { static_cast<std::uint8_t>(crc >> 24), static_cast<std::uint8_t>(crc >> 16), static_cast<std::uint8_t>(crc >> 8), static_cast<std::uint8_t>(crc) };
		a_out.write(reinterpret_cast<const char*>(crcBytes), 4);
	}

	void WritePng(const std::string& a_path, const texture::Image& a_image)
	{
		std::vector<std::uint8_t> raw;
		for (std::uint32_t y = 0; y < a_image.height; ++y) {
			raw.push_back(0);
			raw.insert(raw.end(), a_image.rgba.begin() + static_cast<std::ptrdiff_t>(y) * a_image.width * 4, a_image.rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * a_image.width * 4);
		}
		uLongf size = compressBound(static_cast<uLong>(raw.size()));
		std::vector<std::uint8_t> packed(size);
		compress(packed.data(), &size, raw.data(), static_cast<uLong>(raw.size()));
		packed.resize(size);
		std::ofstream out(a_path, std::ios::binary);
		const std::uint8_t signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
		out.write(reinterpret_cast<const char*>(signature), 8);
		std::vector<std::uint8_t> header(13, 0);
		for (int i = 0; i < 4; ++i) {
			header[i] = static_cast<std::uint8_t>(a_image.width >> (24 - 8 * i));
			header[4 + i] = static_cast<std::uint8_t>(a_image.height >> (24 - 8 * i));
		}
		header[8] = 8;
		header[9] = 6;
		Chunk(out, "IHDR", header);
		Chunk(out, "IDAT", packed);
		Chunk(out, "IEND", {});
	}
}

int main(int argc, char** argv)
{
	if (argc < 7) {
		std::cerr << "usage: tesworld-render <out.png> <height> <yaw> <pitch> <data folder>... -- <mesh>...\n";
		return 2;
	}
	const std::string outPath = argv[1];
	const auto height = static_cast<std::uint32_t>(std::stoul(argv[2]));
	const float yaw = std::stof(argv[3]) * std::numbers::pi_v<float> / 180.0f;
	const float pitch = std::stof(argv[4]) * std::numbers::pi_v<float> / 180.0f;
	std::vector<std::filesystem::path> roots, archives;
	int i = 5;
	for (; i < argc && std::string(argv[i]) != "--"; ++i) {
		roots.emplace_back(argv[i]);
		for (const auto& entry : std::filesystem::directory_iterator(argv[i])) {
			if (vfs::NormalizePath(entry.path().extension().string()) == ".bsa") archives.push_back(entry.path());
		}
	}
	std::ranges::sort(archives);
	const vfs::FileSystem files({ .looseRoots = roots, .archives = archives });

	std::vector<nif::Model> models;
	for (++i; i < argc; ++i) {
		const auto bytes = std::filesystem::exists(argv[i]) ? std::optional(platform::ReadFileBytes(argv[i])) : files.Read(argv[i]);
		std::string error;
		auto model = bytes ? nif::ReadModel(*bytes, &error) : std::nullopt;
		if (!model) {
			std::cerr << "can't read " << argv[i] << ' ' << error << '\n';
			continue;
		}
		models.push_back(std::move(*model));
	}
	std::vector<render::DrawItem> items;
	for (const auto& model : models) items.push_back(render::DrawItem{ .model = &model });

	// Looking along forward: yaw turns it from +Y toward +X, pitch tips it down.
	const Vec3 forward = Normalize({ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) });
	const Vec3 right = Normalize(Cross(forward, { 0, 0, 1 }), { 1, 0, 0 });
	const Vec3 up = Cross(right, forward);
	const auto [min, max] = render::Bounds(items, true);
	if (!(max.x >= min.x)) return 1;
	// The box's corners seen across and up: the view fits them.
	float r0 = 1e30f, r1 = -1e30f, u0 = 1e30f, u1 = -1e30f;
	const Vec3 center{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f };
	for (int c = 0; c < 8; ++c) {
		const Vec3 corner{ (c & 1) ? max.x : min.x, (c & 2) ? max.y : min.y, (c & 4) ? max.z : min.z };
		const auto d = corner - center;
		r0 = (std::min)(r0, Dot(d, right));
		r1 = (std::max)(r1, Dot(d, right));
		u0 = (std::min)(u0, Dot(d, up));
		u1 = (std::max)(u1, Dot(d, up));
	}
	const render::Camera camera{
		.center = center + right * ((r0 + r1) * 0.5f) + up * ((u0 + u1) * 0.5f),
		.right = right,
		.up = up,
		.forward = forward,
		.width = r1 - r0,
		.height = u1 - u0,
	};
	const auto width = (std::max)(1u, static_cast<std::uint32_t>(static_cast<float>(height) * camera.width / camera.height));

	std::map<std::string, std::unique_ptr<texture::MipChain>> textures;
	const render::TextureSource source = [&](const std::string& a_path) -> const texture::MipChain* {
		auto key = vfs::NormalizePath(a_path);
		if (!key.starts_with("textures\\")) key = "textures\\" + key;
		auto [it, inserted] = textures.try_emplace(key);
		if (inserted) {
			if (const auto data = files.Read(key)) {
				if (auto chain = texture::DecodeDds(*data)) it->second = std::make_unique<texture::MipChain>(std::move(*chain));
			}
			if (!it->second) std::cerr << "missing texture " << key << '\n';
		}
		return it->second.get();
	};
	render::CpuRenderer renderer;
	auto frame = renderer.Render(items, camera, render::RenderOptions{ .width = width, .height = height, .supersample = 2, .skinned = true }, source);
	// On grey, to see the edges.
	for (std::size_t p = 0; p < frame.color.rgba.size(); p += 4) {
		const float a = frame.color.rgba[p + 3] / 255.0f;
		for (int k = 0; k < 3; ++k) frame.color.rgba[p + k] = static_cast<std::uint8_t>(frame.color.rgba[p + k] * a + 96 * (1.0f - a));
		frame.color.rgba[p + 3] = 255;
	}
	WritePng(outPath, frame.color);
	std::cout << "rendered " << models.size() << " meshes, " << width << "x" << height << " -> " << outPath << '\n';
	return 0;
}