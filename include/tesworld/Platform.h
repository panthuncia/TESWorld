#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tesworld::platform
{
	[[nodiscard]] std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& a_path);

	// Paths as UTF-8 text and back. On Windows std::filesystem::path::string() uses the ANSI code page and throws on
	// characters outside it, and a path made from a std::string reads it in that code page.
	[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& a_path);
	[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view a_text);

	// A read-only view of a whole file: memory-mapped where the OS allows, else read into memory. Safe to read from
	// any thread after construction; the owner decides when the mapping ends.
	class FileByteView
	{
	public:
		FileByteView() = default;
		FileByteView(const FileByteView&) = delete;
		FileByteView& operator=(const FileByteView&) = delete;
		FileByteView(FileByteView&& a_rhs) noexcept;
		FileByteView& operator=(FileByteView&& a_rhs) noexcept;
		~FileByteView();

		[[nodiscard]] std::span<const std::uint8_t> Span() const noexcept { return _bytes; }

	private:
		void Close() noexcept;

		std::vector<std::uint8_t> _ownedBytes;
		std::span<const std::uint8_t> _bytes;
		// The OS handles, kept opaque so this header needs no platform includes.
		void* _mappedView{ nullptr };
		std::size_t _mappedSize{ 0 };
		void* _mappingHandle{ nullptr };
		void* _fileHandle{ nullptr };

		friend FileByteView MapFileBytes(const std::filesystem::path& a_path);
	};

	[[nodiscard]] FileByteView MapFileBytes(const std::filesystem::path& a_path);
}
