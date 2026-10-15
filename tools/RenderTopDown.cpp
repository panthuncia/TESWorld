// tesworld-topdown <out.png> <size> <data folder>... -- <mesh>...
// Renders meshes seen from straight above, textured and lit by their normals, into one PNG: a check that LOD
// meshes, their UVs and their textures line up. Game paths are looked up in the data folders (later ones win),
// including every BSA directly in them.

#include "tesworld/Nif.h"
#include "tesworld/Platform.h"
#include "tesworld/Texture.h"
#include "tesworld/Vfs.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>

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
	if (argc < 5) {
		std::cerr << "usage: tesworld-topdown <out.png> <size> <data folder>... -- <mesh>...\n";
		return 2;
	}
	const std::string outPath = argv[1];
	const auto size = static_cast<std::uint32_t>(std::stoul(argv[2]));
	std::vector<std::filesystem::path> roots, archives;
	int i = 3;
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

	Vec3 min{ 1e30f, 1e30f, 1e30f }, max{ -1e30f, -1e30f, -1e30f };
	for (const auto& model : models) {
		for (const auto& shape : model.shapes) {
			for (const auto& p : shape.positions) {
				min = { (std::min)(min.x, p.x), (std::min)(min.y, p.y), (std::min)(min.z, p.z) };
				max = { (std::max)(max.x, p.x), (std::max)(max.y, p.y), (std::max)(max.z, p.z) };
			}
		}
	}
	const float extent = (std::max)(max.x - min.x, max.y - min.y);
	if (extent <= 0) return 1;
	const float scale = static_cast<float>(size) / extent;

	texture::Image image(size, size);
	std::vector<float> depth(static_cast<std::size_t>(size) * size, -std::numeric_limits<float>::max());
	std::map<std::string, std::shared_ptr<texture::MipChain>> textures;
	const auto load = [&](const std::string& a_path) -> const texture::MipChain* {
		auto path = vfs::NormalizePath(a_path);
		if (path.empty()) return nullptr;
		if (!path.starts_with("textures\\")) path = "textures\\" + path;
		auto [it, inserted] = textures.try_emplace(path);
		if (inserted) {
			if (const auto bytes = files.Read(path)) {
				if (auto chain = texture::DecodeDds(*bytes)) it->second = std::make_shared<texture::MipChain>(std::move(*chain));
			}
			if (!it->second) std::cerr << "missing texture " << path << '\n';
		}
		return it->second.get();
	};

	const Vec3 light = Normalize({ -0.4f, 0.5f, 0.75f });
	for (const auto& model : models) {
		for (const auto& shape : model.shapes) {
			const auto* diffuse = load(shape.shader.textures[0]);
			const auto* normalMap = load(shape.shader.textures[1]);
			const bool modelSpace = shape.shader.ModelSpaceNormals();
			for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3) {
				Vec3 p[3];
				Vec2 s[3];
				for (int k = 0; k < 3; ++k) {
					p[k] = shape.positions[shape.indices[t + k]];
					s[k] = { (p[k].x - min.x) * scale, (max.y - p[k].y) * scale };
				}
				const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
				if (std::abs(area) < 1e-6f) continue;
				const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ s[0].x, s[1].x, s[2].x }))));
				const int x1 = (std::min)(static_cast<int>(size) - 1, static_cast<int>(std::ceil((std::max)({ s[0].x, s[1].x, s[2].x }))));
				const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ s[0].y, s[1].y, s[2].y }))));
				const int y1 = (std::min)(static_cast<int>(size) - 1, static_cast<int>(std::ceil((std::max)({ s[0].y, s[1].y, s[2].y }))));
				for (int y = y0; y <= y1; ++y) {
					for (int x = x0; x <= x1; ++x) {
						const float px = x + 0.5f, py = y + 0.5f;
						const float w0 = ((s[1].x - px) * (s[2].y - py) - (s[2].x - px) * (s[1].y - py)) / area;
						const float w1 = ((s[2].x - px) * (s[0].y - py) - (s[0].x - px) * (s[2].y - py)) / area;
						const float w2 = 1 - w0 - w1;
						if (w0 < 0 || w1 < 0 || w2 < 0) continue;
						const float z = p[0].z * w0 + p[1].z * w1 + p[2].z * w2;
						auto& d = depth[static_cast<std::size_t>(y) * size + x];
						if (z < d) continue;
						Vec4 colour{ 0.6f, 0.6f, 0.6f, 1 };
						Vec2 uv{};
						if (shape.uvs.size() == shape.positions.size()) {
							const auto& a = shape.uvs[shape.indices[t]], &b = shape.uvs[shape.indices[t + 1]], &c = shape.uvs[shape.indices[t + 2]];
							uv = { a.x * w0 + b.x * w1 + c.x * w2, a.y * w0 + b.y * w1 + c.y * w2 };
							if (diffuse) colour = texture::SampleLevel(*diffuse, uv, 1.0f, texture::Address::Wrap);
						}
						if (shape.shader.alphaTest && colour.w < shape.shader.alphaThreshold / 255.0f) continue;
						Vec3 n{ 0, 0, 1 };
						if (modelSpace && normalMap) {
							const auto e = texture::SampleLevel(*normalMap, uv, 0.0f, texture::Address::Wrap);
							n = Normalize({ e.x * 2 - 1, e.z * 2 - 1, e.y * 2 - 1 });
						} else if (shape.normals.size() == shape.positions.size()) {
							const auto& a = shape.normals[shape.indices[t]], &b = shape.normals[shape.indices[t + 1]], &c = shape.normals[shape.indices[t + 2]];
							n = Normalize(a * w0 + b * w1 + c * w2);
						}
						const float shade = 0.35f + 0.65f * (std::max)(0.0f, Dot(n, light));
						d = z;
						image.SetTexel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), { colour.x * shade, colour.y * shade, colour.z * shade, 1 });
					}
				}
			}
		}
	}
	WritePng(outPath, image);
	std::cout << "rendered " << models.size() << " meshes over " << extent << " units -> " << outPath << '\n';
	return 0;
}
