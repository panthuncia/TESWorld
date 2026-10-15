#include "tesworld/Vfs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>

#include <lz4frame.h>
#include <zlib.h>

namespace tesworld::vfs
{
	namespace
	{
		constexpr std::uint32_t kBsaMagic = 0x00415342u;  // "BSA\0"
		constexpr std::uint32_t kFlagDirectoryNames = 0x0001u;
		constexpr std::uint32_t kFlagFileNames = 0x0002u;
		constexpr std::uint32_t kFlagCompressed = 0x0004u;
		constexpr std::uint32_t kFlagEmbeddedNames = 0x0100u;
		constexpr std::uint32_t kSizeCompressionToggle = 0x40000000u;

		template <class T>
		T ReadPod(std::istream& a_input)
		{
			T value{};
			a_input.read(reinterpret_cast<char*>(&value), sizeof(T));
			if (!a_input) {
				throw std::runtime_error("unexpected end of archive");
			}
			return value;
		}
	}

	std::string NormalizePath(std::string_view a_path)
	{
		std::string result;
		NormalizePath(a_path, result);
		return result;
	}

	void NormalizePath(std::string_view a_path, std::string& a_out)
	{
		auto& result = a_out;
		result.clear();
		result.reserve(a_path.size());
		for (const char ch : a_path) {
			if (ch == '\0') {
				break;
			}
			result.push_back(ch == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
		}
		auto first = result.find_first_not_of(" \t\\");
		while (first != std::string::npos && result.compare(first, 2, ".\\") == 0) {
			first = result.find_first_not_of('\\', first + 2);
		}
		const auto last = result.find_last_not_of(" \t");
		if (first == std::string::npos) {
			result.clear();
			return;
		}
		// As substr(first, last - first + 1), in place.
		const auto count = last - first + 1;
		result.erase(0, first);
		if (count < result.size()) result.resize(count);
		if (result.starts_with("data\\")) {
			result.erase(0, 5);
		}
		// Collapse doubled separators.
		result.erase(std::unique(result.begin(), result.end(), [](char a, char b) { return a == '\\' && b == '\\'; }), result.end());
	}

	std::string_view FileName(std::string_view a_path)
	{
		const auto slash = a_path.find_last_of("\\/");
		return slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);
	}

	std::string_view FileStem(std::string_view a_path)
	{
		const auto name = FileName(a_path);
		const auto dot = name.find_last_of('.');
		return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
	}

	std::unique_ptr<BsaArchive> BsaArchive::Open(const std::filesystem::path& a_path, std::string* a_error)
	{
		auto archive = std::unique_ptr<BsaArchive>(new BsaArchive());
		archive->_path = a_path;
		std::error_code ec;
		archive->_fileSize = std::filesystem::file_size(a_path, ec);
		archive->_fileTime = ec ? 0 : std::filesystem::last_write_time(a_path, ec).time_since_epoch().count();
		try {
			std::ifstream input(a_path, std::ios::binary);
			if (!input) {
				throw std::runtime_error("failed to open");
			}

			const auto magic = ReadPod<std::uint32_t>(input);
			archive->_version = ReadPod<std::uint32_t>(input);
			const auto foldersOffset = ReadPod<std::uint32_t>(input);
			archive->_flags = ReadPod<std::uint32_t>(input);
			const auto folderCount = ReadPod<std::uint32_t>(input);
			const auto fileCount = ReadPod<std::uint32_t>(input);
			const auto folderNamesLength = ReadPod<std::uint32_t>(input);
			const auto fileNamesLength = ReadPod<std::uint32_t>(input);
			(void)ReadPod<std::uint32_t>(input);  // file flags

			if (magic != kBsaMagic) {
				throw std::runtime_error("not a BSA archive");
			}
			if (archive->_version != 103u && archive->_version != 104u && archive->_version != 105u) {
				throw std::runtime_error("unsupported BSA version " + std::to_string(archive->_version));
			}
			if ((archive->_flags & kFlagDirectoryNames) == 0 || (archive->_flags & kFlagFileNames) == 0) {
				throw std::runtime_error("archive does not store folder and file names");
			}

			std::vector<std::uint32_t> folderFileCounts(folderCount);
			input.seekg(foldersOffset, std::ios::beg);
			for (std::uint32_t i = 0; i < folderCount; ++i) {
				(void)ReadPod<std::uint64_t>(input);  // name hash
				folderFileCounts[i] = ReadPod<std::uint32_t>(input);
				if (archive->_version == 105u) {
					(void)ReadPod<std::uint32_t>(input);
					(void)ReadPod<std::uint64_t>(input);
				} else {
					(void)ReadPod<std::uint32_t>(input);
				}
			}

			struct FileRecord
			{
				std::uint32_t folder{ 0 };
				Entry entry;
			};
			std::vector<std::string> folders(folderCount);
			std::vector<FileRecord> files;
			files.reserve(fileCount);
			for (std::uint32_t folderIndex = 0; folderIndex < folderCount; ++folderIndex) {
				const auto nameSize = ReadPod<std::uint8_t>(input);
				auto& folder = folders[folderIndex];
				folder.assign(nameSize, '\0');
				if (nameSize > 0) {
					input.read(folder.data(), nameSize);
					if (!input) {
						throw std::runtime_error("failed to read folder name");
					}
					if (!folder.empty() && folder.back() == '\0') {
						folder.pop_back();
					}
				}
				for (std::uint32_t i = 0; i < folderFileCounts[folderIndex]; ++i) {
					(void)ReadPod<std::uint64_t>(input);  // name hash
					FileRecord record{ .folder = folderIndex };
					record.entry.size = ReadPod<std::uint32_t>(input);
					record.entry.offset = ReadPod<std::uint32_t>(input);
					files.push_back(std::move(record));
				}
			}

			// The names into one buffer (the header's lengths are its size, give or take), each file a slot of the table.
			auto& names = archive->_names;
			auto& slots = archive->_slots;
			names.reserve(std::size_t{ folderNamesLength } + fileNamesLength + files.size());
			slots.reserve(files.size());
			std::string name, joined, normalized;
			for (const auto& file : files) {
				std::getline(input, name, '\0');
				if (!input || name.empty()) {
					throw std::runtime_error("failed to read file name");
				}
				const auto& folder = folders[file.folder];
				joined = folder;
				if (!folder.empty()) joined.push_back('\\');
				joined += name;
				NormalizePath(joined, normalized);
				if (normalized.empty()) continue;
				slots.push_back(Slot{ .hash = std::hash<std::string_view>{}(normalized), .name = static_cast<std::uint32_t>(names.size()),
					.length = static_cast<std::uint32_t>(normalized.size()), .entry = file.entry });
				names += normalized;
			}
			// By hash; of two files at one path the later is kept, as before.
			std::ranges::stable_sort(slots, {}, &Slot::hash);
			std::size_t kept = 0;
			for (std::size_t i = 0; i < slots.size(); ++i) {
				bool later = false;
				for (std::size_t j = i + 1; j < slots.size() && slots[j].hash == slots[i].hash && !later; ++j) {
					later = archive->NameOf(slots[j]) == archive->NameOf(slots[i]);
				}
				if (!later) slots[kept++] = slots[i];
			}
			slots.resize(kept);
			return archive;
		} catch (const std::exception& ex) {
			if (a_error) {
				*a_error = ex.what();
			}
			return nullptr;
		}
	}

	const BsaArchive::Entry* BsaArchive::Find(std::string_view a_normalizedPath) const
	{
		const auto hash = std::hash<std::string_view>{}(a_normalizedPath);
		for (auto it = std::ranges::lower_bound(_slots, hash, {}, &Slot::hash); it != _slots.end() && it->hash == hash; ++it) {
			if (NameOf(*it) == a_normalizedPath) return &it->entry;
		}
		return nullptr;
	}

	bool BsaArchive::Contains(std::string_view a_normalizedPath) const
	{
		return Find(a_normalizedPath) != nullptr;
	}

	std::optional<FileStamp> BsaArchive::Stamp(std::string_view a_normalizedPath) const
	{
		const auto* entry = Find(a_normalizedPath);
		if (!entry) {
			return std::nullopt;
		}
		return FileStamp{ .origin = Origin::Archive, .container = _path, .size = entry->size, .time = _fileTime, .offset = entry->offset,
			.containerSize = _fileSize };
	}

	std::vector<std::string_view> BsaArchive::Paths() const
	{
		std::vector<std::string_view> paths;
		paths.reserve(_slots.size());
		for (const auto& slot : _slots) {
			paths.push_back(NameOf(slot));
		}
		return paths;
	}

	std::optional<std::vector<std::uint8_t>> BsaArchive::ReadRange(std::string_view a_normalizedPath, std::uint64_t a_offset, std::uint64_t a_length) const
	{
		const auto* entry = Find(a_normalizedPath);
		if (!entry) return std::nullopt;
		const auto size = entry->size & ~kSizeCompressionToggle;
		const bool toggled = (entry->size & kSizeCompressionToggle) != 0;
		const bool compressed = toggled != ((_flags & kFlagCompressed) != 0);
		const auto slice = [&](std::vector<std::uint8_t> a_bytes) {
			const auto from = (std::min<std::uint64_t>)(a_offset, a_bytes.size());
			const auto to = from + (std::min<std::uint64_t>)(a_length, a_bytes.size() - from);
			return std::vector<std::uint8_t>(a_bytes.begin() + static_cast<std::ptrdiff_t>(from), a_bytes.begin() + static_cast<std::ptrdiff_t>(to));
		};
		if (compressed) {
			auto whole = Read(a_normalizedPath);
			if (!whole) return std::nullopt;
			return slice(std::move(*whole));
		}
		try {
			std::ifstream input(_path, std::ios::binary);
			if (!input) return std::nullopt;
			input.seekg(entry->offset, std::ios::beg);
			std::uint64_t skip = 0;
			if ((_flags & kFlagEmbeddedNames) != 0) {
				char length = 0;
				if (!input.read(&length, 1)) return std::nullopt;
				skip = static_cast<std::uint64_t>(static_cast<std::uint8_t>(length)) + 1u;
			}
			if (skip > size) return std::nullopt;
			const auto stored = size - skip;
			const auto from = (std::min<std::uint64_t>)(a_offset, stored);
			std::vector<std::uint8_t> bytes((std::min<std::uint64_t>)(a_length, stored - from));
			input.seekg(static_cast<std::streamoff>(entry->offset + skip + from), std::ios::beg);
			if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
			return bytes;
		} catch (const std::exception&) {
			return std::nullopt;
		}
	}

	std::optional<std::vector<std::uint8_t>> BsaArchive::Read(std::string_view a_normalizedPath, std::string* a_error) const
	{
		const auto* entry = Find(a_normalizedPath);
		if (!entry) {
			if (a_error) *a_error = "not in archive";
			return std::nullopt;
		}

		try {
			auto size = entry->size & ~kSizeCompressionToggle;
			const bool toggled = (entry->size & kSizeCompressionToggle) != 0;
			const bool compressed = toggled != ((_flags & kFlagCompressed) != 0);

			std::ifstream input(_path, std::ios::binary);
			if (!input) {
				throw std::runtime_error("failed to open archive");
			}
			input.seekg(entry->offset, std::ios::beg);
			std::vector<std::uint8_t> raw(size);
			input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
			if (!input) {
				throw std::runtime_error("failed to read archive payload");
			}

			std::size_t embedded = 0;
			if ((_flags & kFlagEmbeddedNames) != 0) {
				if (raw.empty()) {
					throw std::runtime_error("embedded-name payload is empty");
				}
				embedded = static_cast<std::size_t>(raw.front()) + 1u;
				if (embedded > raw.size()) {
					throw std::runtime_error("embedded-name payload is truncated");
				}
			}
			if (!compressed) {
				// The file as read, without its name: no second buffer.
				raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(embedded));
				return raw;
			}
			auto payload = std::span<const std::uint8_t>(raw).subspan(embedded);

			if (payload.size() < sizeof(std::uint32_t)) {
				throw std::runtime_error("compressed payload is missing its size");
			}
			std::uint32_t expected{};
			std::memcpy(&expected, payload.data(), sizeof(expected));
			payload = payload.subspan(sizeof(std::uint32_t));
			std::vector<std::uint8_t> output(expected);
			if (_version == 105u) {
				LZ4F_decompressionContext_t context = nullptr;
				auto result = LZ4F_createDecompressionContext(&context, LZ4F_VERSION);
				if (LZ4F_isError(result)) {
					throw std::runtime_error(std::string("LZ4: ") + LZ4F_getErrorName(result));
				}
				std::size_t outSize = output.size();
				std::size_t inSize = payload.size();
				result = LZ4F_decompress(context, output.data(), &outSize, payload.data(), &inSize, nullptr);
				const auto freed = LZ4F_freeDecompressionContext(context);
				if (LZ4F_isError(result) || LZ4F_isError(freed) || outSize != output.size()) {
					throw std::runtime_error("LZ4 decompression failed");
				}
			} else {
				uLongf outSize = static_cast<uLongf>(output.size());
				const auto result = uncompress(output.data(), &outSize, payload.data(), static_cast<uLong>(payload.size()));
				if (result != Z_OK || outSize != output.size()) {
					throw std::runtime_error("zlib decompression failed");
				}
			}
			return output;
		} catch (const std::exception& ex) {
			if (a_error) *a_error = ex.what();
			return std::nullopt;
		}
	}
}
