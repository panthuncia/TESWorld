#include "tesworld/Nif.h"

#include <algorithm>
#include <map>
#include <spanstream>
#include <sstream>
#include <stdexcept>

#include <NifFile.hpp>

namespace tesworld::nif
{
	namespace
	{
		constexpr std::size_t kMaxBytes = 256u * 1024u * 1024u;
		std::uint32_t Read32(std::span<const std::uint8_t> a_bytes, std::size_t& a_offset)
		{
			if (a_offset > a_bytes.size() || a_bytes.size() - a_offset < 4) throw std::runtime_error("truncated texture-set payload");
			std::uint32_t value = 0;
			for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(a_bytes[a_offset++]) << shift;
			return value;
		}
		void Write32(std::vector<std::uint8_t>& a_bytes, std::uint32_t a_value)
		{
			for (unsigned shift = 0; shift < 32; shift += 8) a_bytes.push_back(static_cast<std::uint8_t>(a_value >> shift));
		}
		bool SafeReplacement(const std::string& a_path)
		{
			if (a_path.empty()) return true;
			if (a_path.size() > 4096 || a_path.find(':') != std::string::npos ||
				std::ranges::any_of(a_path, [](unsigned char c) { return c < 32 || c == 127; })) return false;
			std::size_t start = 0;
			while (start <= a_path.size()) {
				const auto end = a_path.find_first_of("/\\", start);
				const auto part = a_path.substr(start, end == std::string::npos ? end : end - start);
				if (part.empty() || part == "." || part == "..") return false;
				if (end == std::string::npos) break;
				start = end + 1;
			}
			return true;
		}
	}

	std::optional<std::vector<std::uint8_t>> RewriteTextureSets(std::span<const std::uint8_t> a_bytes,
		std::span<const TextureSetEdit> a_edits, std::string* a_error)
	{
		try {
			if (a_bytes.size() > kMaxBytes || a_edits.size() > 100000) throw std::runtime_error("texture rewrite exceeds inspection limits");
			const auto lineEnd = std::ranges::find(a_bytes, '\n');
			const auto endianOffset = static_cast<std::size_t>(lineEnd - a_bytes.begin()) + 5;
			if (endianOffset >= a_bytes.size() || a_bytes[endianOffset] != 1) throw std::runtime_error("texture rewrite requires little-endian Skyrim SE NIF");
			std::ispanstream stream(std::span<const char>(reinterpret_cast<const char*>(a_bytes.data()), a_bytes.size()));
			nifly::NiHeader header;
			nifly::NiIStream input(&stream, &header);
			header.Get(input);
			if (!stream || !header.IsValid() || header.GetVersion().File() != nifly::V20_2_0_7 ||
				header.GetVersion().User() != 12 || header.GetVersion().Stream() != 100 || header.GetNumBlocks() > 100000)
				throw std::runtime_error("texture rewrite requires Skyrim SE NIF 20.2.0.7 / 12 / 100");
			const auto headerSize = static_cast<std::size_t>(stream.tellg());
			std::ostringstream serialized(std::ios::binary);
			nifly::NiOStream output(&serialized, &header);
			header.Put(output);
			const auto canonical = serialized.str();
			if (canonical.size() != headerSize || !std::equal(canonical.begin(), canonical.end(), reinterpret_cast<const char*>(a_bytes.data())))
				throw std::runtime_error("NIF header cannot be preserved exactly");
			const auto sizeTable = static_cast<std::size_t>(header.GetBlockSizeStreamPos());
			if (sizeTable > headerSize || header.GetNumBlocks() > (headerSize - sizeTable) / 4)
				throw std::runtime_error("invalid NIF block-size table");
			std::map<std::uint32_t, std::map<std::uint32_t, const TextureSetEdit*>> edits;
			for (const auto& edit : a_edits) {
				if (edit.block >= header.GetNumBlocks() || edit.slot >= kTextureSlots || !SafeReplacement(edit.replacement) ||
					header.GetBlockTypeStringById(edit.block) != "BSShaderTextureSet" || !edits[edit.block].emplace(edit.slot, &edit).second)
					throw std::runtime_error("invalid, duplicate or unsupported texture-set edit");
			}
			std::vector<std::uint8_t> result(a_bytes.begin(), a_bytes.begin() + headerSize);
			std::size_t offset = headerSize;
			for (std::uint32_t id = 0; id < header.GetNumBlocks(); ++id) {
				const auto size = header.GetBlockSize(id);
				if (offset > a_bytes.size() || size > a_bytes.size() - offset) throw std::runtime_error("NIF block extends beyond input");
				const auto block = a_bytes.subspan(offset, size);
				offset += size;
				const auto found = edits.find(id);
				if (found == edits.end()) {
					if (result.size() > kMaxBytes - block.size()) throw std::runtime_error("prepared NIF exceeds size limit");
					result.insert(result.end(), block.begin(), block.end()); continue;
				}
				std::size_t at = 0;
				const auto count = Read32(block, at);
				if (count > kTextureSlots) throw std::runtime_error("unsupported Skyrim texture-set slot count");
				std::vector<std::uint8_t> replacement;
				Write32(replacement, count);
				for (std::uint32_t slot = 0; slot < count; ++slot) {
					const auto length = Read32(block, at);
					if (length > block.size() - at) throw std::runtime_error("truncated texture string");
					std::string text(reinterpret_cast<const char*>(block.data() + at), length);
					at += length;
					if (const auto change = found->second.find(slot); change != found->second.end()) {
						const bool terminated = !text.empty() && text.back() == 0;
						const auto value = std::string_view(text).substr(0, text.size() - (terminated ? 1 : 0));
						if (value != change->second->expected) throw std::runtime_error("texture edit does not match observed source string");
						text = change->second->replacement;
						if (terminated) text.push_back(0);
					}
					Write32(replacement, static_cast<std::uint32_t>(text.size()));
					replacement.insert(replacement.end(), text.begin(), text.end());
				}
				if (at != block.size() || found->second.rbegin()->first >= count) throw std::runtime_error("invalid texture-set block or slot");
				if (replacement.size() > kMaxBytes || result.size() > kMaxBytes - replacement.size()) throw std::runtime_error("prepared NIF exceeds size limit");
				const auto newSize = static_cast<std::uint32_t>(replacement.size());
				for (unsigned byte = 0; byte < 4; ++byte) result[sizeTable + id * 4 + byte] = static_cast<std::uint8_t>(newSize >> (byte * 8));
				result.insert(result.end(), replacement.begin(), replacement.end());
			}
			auto footer = offset;
			const auto roots = Read32(a_bytes, footer);
			if (!roots || roots > 100000) throw std::runtime_error("invalid NIF root count");
			for (std::uint32_t root = 0; root < roots; ++root)
				if (Read32(a_bytes, footer) >= header.GetNumBlocks()) throw std::runtime_error("invalid NIF root");
			if (result.size() > kMaxBytes || a_bytes.size() - offset > kMaxBytes - result.size()) throw std::runtime_error("prepared NIF exceeds size limit");
			result.insert(result.end(), a_bytes.begin() + offset, a_bytes.end());
			return result;
		} catch (const std::exception& error) {
			if (a_error) *a_error = error.what();
			return std::nullopt;
		}
	}
}