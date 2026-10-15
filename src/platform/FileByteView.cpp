#include "tesworld/Platform.h"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tesworld::platform
{
	std::string PathToUtf8(const std::filesystem::path& a_path)
	{
		const auto text = a_path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::filesystem::path PathFromUtf8(std::string_view a_text)
	{
		return std::filesystem::path(std::u8string(a_text.begin(), a_text.end()));
	}

	std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& a_path)
	{
		std::ifstream input(a_path, std::ios::binary);
		if (!input) throw std::runtime_error("failed to open file: " + PathToUtf8(a_path));
		input.seekg(0, std::ios::end);
		const auto size = input.tellg();
		if (size < 0) throw std::runtime_error("failed to size file: " + PathToUtf8(a_path));
		input.seekg(0, std::ios::beg);
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		if (!bytes.empty()) {
			input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!input) throw std::runtime_error("failed to read file: " + PathToUtf8(a_path));
		}
		return bytes;
	}

	FileByteView::FileByteView(FileByteView&& a_rhs) noexcept { *this = std::move(a_rhs); }

	FileByteView& FileByteView::operator=(FileByteView&& a_rhs) noexcept
	{
		if (this != std::addressof(a_rhs)) {
			Close();
			_ownedBytes = std::move(a_rhs._ownedBytes);
			_bytes = _ownedBytes.empty() ? a_rhs._bytes : std::span<const std::uint8_t>{ _ownedBytes };
			_mappedView = std::exchange(a_rhs._mappedView, nullptr);
			_mappedSize = std::exchange(a_rhs._mappedSize, 0);
			_mappingHandle = std::exchange(a_rhs._mappingHandle, nullptr);
			_fileHandle = std::exchange(a_rhs._fileHandle, nullptr);
			a_rhs._bytes = {};
		}
		return *this;
	}

	FileByteView::~FileByteView() { Close(); }

	void FileByteView::Close() noexcept
	{
#ifdef _WIN32
		if (_mappedView) UnmapViewOfFile(_mappedView);
		if (_mappingHandle) CloseHandle(_mappingHandle);
		if (_fileHandle) CloseHandle(_fileHandle);
#else
		if (_mappedView) munmap(_mappedView, _mappedSize);
#endif
		_mappedView = nullptr;
		_mappedSize = 0;
		_mappingHandle = nullptr;
		_fileHandle = nullptr;
		_bytes = {};
	}

	FileByteView MapFileBytes(const std::filesystem::path& a_path)
	{
		FileByteView result;
#ifdef _WIN32
		const auto file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			result._fileHandle = file;
			LARGE_INTEGER fileSize{};
			if (GetFileSizeEx(file, std::addressof(fileSize)) && fileSize.QuadPart > 0) {
				result._mappingHandle = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
				if (result._mappingHandle) {
					result._mappedView = MapViewOfFile(result._mappingHandle, FILE_MAP_READ, 0, 0, 0);
					if (result._mappedView) {
						result._mappedSize = static_cast<std::size_t>(fileSize.QuadPart);
						result._bytes = { static_cast<const std::uint8_t*>(result._mappedView), result._mappedSize };
						return result;
					}
				}
			}
			result.Close();
		}
#else
		const int fd = ::open(a_path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			struct stat info{};
			if (::fstat(fd, &info) == 0 && info.st_size > 0) {
				void* view = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
				if (view != MAP_FAILED) {
					::close(fd);
					result._mappedView = view;
					result._mappedSize = static_cast<std::size_t>(info.st_size);
					result._bytes = { static_cast<const std::uint8_t*>(view), result._mappedSize };
					return result;
				}
			}
			::close(fd);
		}
#endif
		result._ownedBytes = ReadFileBytes(a_path);
		result._bytes = result._ownedBytes;
		return result;
	}
}
