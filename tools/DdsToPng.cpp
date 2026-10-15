// tesworld-dds2png <in.dds> <out.png> [level]
// Writes one level of a DDS as an RGBA PNG, for looking at generated textures.

#include "tesworld/Platform.h"
#include "tesworld/Texture.h"

#include <cstring>
#include <fstream>
#include <iostream>

#include <zlib.h>

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
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::cerr << "usage: tesworld-dds2png <in.dds> <out.png> [level]\n";
		return 2;
	}
	const auto bytes = tesworld::platform::ReadFileBytes(argv[1]);
	std::string error;
	const auto chain = tesworld::texture::DecodeDds(bytes, {}, &error);
	if (!chain) {
		std::cerr << error << '\n';
		return 1;
	}
	const auto level = argc > 3 ? static_cast<std::size_t>(std::stoul(argv[3])) : 0;
	const auto& image = chain->levels.at((std::min)(level, chain->levels.size() - 1));

	std::vector<std::uint8_t> raw;
	raw.reserve((static_cast<std::size_t>(image.width) * 4 + 1) * image.height);
	for (std::uint32_t y = 0; y < image.height; ++y) {
		raw.push_back(0);
		raw.insert(raw.end(), image.rgba.begin() + static_cast<std::ptrdiff_t>(y) * image.width * 4, image.rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * image.width * 4);
	}
	uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
	std::vector<std::uint8_t> packed(packedSize);
	compress(packed.data(), &packedSize, raw.data(), static_cast<uLong>(raw.size()));
	packed.resize(packedSize);

	std::ofstream out(argv[2], std::ios::binary);
	const std::uint8_t signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	out.write(reinterpret_cast<const char*>(signature), 8);
	std::vector<std::uint8_t> header(13, 0);
	for (int i = 0; i < 4; ++i) {
		header[i] = static_cast<std::uint8_t>(image.width >> (24 - 8 * i));
		header[4 + i] = static_cast<std::uint8_t>(image.height >> (24 - 8 * i));
	}
	header[8] = 8;   // bit depth
	header[9] = 6;   // RGBA
	Chunk(out, "IHDR", header);
	Chunk(out, "IDAT", packed);
	Chunk(out, "IEND", {});
	std::cout << image.width << "x" << image.height << " -> " << argv[2] << '\n';
	return 0;
}
