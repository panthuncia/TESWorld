#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tesworld::vfs
{
	// A game path as the archives and the game compare them: lower case, backslashes, no leading separator or "data\".
	[[nodiscard]] std::string NormalizePath(std::string_view a_path);
	// The same into a_out, whose capacity is used again: a lookup that keeps a buffer allocates nothing.
	void NormalizePath(std::string_view a_path, std::string& a_out);
	// A game path's file name and its stem, split at either separator on every OS (std::filesystem only splits at '/'
	// off Windows).
	[[nodiscard]] std::string_view FileName(std::string_view a_path);
	[[nodiscard]] std::string_view FileStem(std::string_view a_path);

	enum class Origin
	{
		Loose,
		Archive
	};

	// What tells one version of a file from another without reading it: where it comes from, its size and its time (an
	// archive's file: the archive's time, and the file's place in it). Equal stamps mean the same file, as far as a tool
	// that regenerates from it can tell cheaply.
	struct FileStamp
	{
		Origin origin{ Origin::Loose };
		std::filesystem::path container;
		std::uint64_t size{ 0 };
		std::int64_t time{ 0 };
		// An archive's file: its place in the archive, and the archive's own size.
		std::uint64_t offset{ 0 };
		std::uint64_t containerSize{ 0 };

		friend bool operator==(const FileStamp&, const FileStamp&) = default;
	};

	// A Skyrim SE / LE BSA (versions 103 to 105). Thread-safe after Open: each read opens the archive file anew.
	class BsaArchive
	{
	public:
		[[nodiscard]] static std::unique_ptr<BsaArchive> Open(const std::filesystem::path& a_path, std::string* a_error = nullptr);

		[[nodiscard]] const std::filesystem::path& Path() const { return _path; }
		[[nodiscard]] bool Contains(std::string_view a_normalizedPath) const;
		[[nodiscard]] std::optional<std::vector<std::uint8_t>> Read(std::string_view a_normalizedPath, std::string* a_error = nullptr) const;
		// a_length bytes from a_offset of a file (fewer at its end). A compressed file is read whole for it.
		[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadRange(std::string_view a_normalizedPath, std::uint64_t a_offset, std::uint64_t a_length) const;
		// Views of the archive's own paths, valid while it is.
		[[nodiscard]] std::vector<std::string_view> Paths() const;
		[[nodiscard]] std::size_t FileCount() const { return _slots.size(); }
		[[nodiscard]] std::optional<FileStamp> Stamp(std::string_view a_normalizedPath) const;

	private:
		struct Entry
		{
			std::uint32_t size{ 0 };
			std::uint32_t offset{ 0 };
		};
		// A file: its path's hash and place in _names, and where it is in the archive.
		struct Slot
		{
			std::uint64_t hash{ 0 };
			std::uint32_t name{ 0 };
			std::uint32_t length{ 0 };
			Entry entry;
		};

		[[nodiscard]] const Entry* Find(std::string_view a_normalizedPath) const;
		[[nodiscard]] std::string_view NameOf(const Slot& a_slot) const { return std::string_view(_names).substr(a_slot.name, a_slot.length); }

		std::filesystem::path _path;
		std::uint32_t _version{ 0 };
		std::uint32_t _flags{ 0 };
		// The archive file's own size and time, as it was opened.
		std::uint64_t _fileSize{ 0 };
		std::int64_t _fileTime{ 0 };
		// Every path, normalized, one after another: one allocation for the archive's names, not one each.
		std::string _names;
		// Sorted by hash.
		std::vector<Slot> _slots;
	};

	struct FileSystemOptions
	{
		// Folders of loose files, lowest priority first: the game's Data folder, or MO2's mods in priority order with
		// overwrite last. Indexed once, so later lookups are case-insensitive on every OS.
		std::vector<std::filesystem::path> looseRoots;
		// Archives, lowest priority first, in the order the game loads them: the INI's archive lists, then each
		// plugin's own archives in load order. Loose files win over every archive.
		std::vector<std::filesystem::path> archives;
	};

	struct Located
	{
		Origin origin{ Origin::Loose };
		std::filesystem::path container;
	};

	// The files the game sees. Thread-safe.
	class FileSystem
	{
	public:
		explicit FileSystem(FileSystemOptions a_options);
		~FileSystem();
		FileSystem(const FileSystem&) = delete;
		FileSystem& operator=(const FileSystem&) = delete;

		[[nodiscard]] bool Exists(std::string_view a_path) const;
		[[nodiscard]] std::optional<Located> Locate(std::string_view a_path) const;
		[[nodiscard]] std::optional<std::vector<std::uint8_t>> Read(std::string_view a_path) const;
		// a_length bytes from a_offset of the file the game would read (fewer at its end): a large texture's small mips
		// without the rest (texture::ReadDds).
		[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadRange(std::string_view a_path, std::uint64_t a_offset, std::uint64_t a_length) const;
		// The stamp of the file the game would read; nullopt when there's none. Loose files' sizes and times are those
		// seen when the folders were indexed.
		[[nodiscard]] std::optional<FileStamp> Stamp(std::string_view a_path) const;
		// Every path under a folder (normalized, e.g. "textures\\terrain\\lodgen\\") with an extension (e.g. ".dds";
		// empty for any), sorted.
		[[nodiscard]] std::vector<std::string> Enumerate(std::string_view a_folder, std::string_view a_extension = {}) const;
		[[nodiscard]] const std::vector<std::string>& ArchiveErrors() const { return _archiveErrors; }

	private:
		struct LooseFile
		{
			std::filesystem::path path;
			std::uint64_t size{ 0 };
			std::int64_t time{ 0 };
		};

		// Looked up by a view, without a string made for it.
		struct PathHash
		{
			using is_transparent = void;
			std::size_t operator()(std::string_view a_path) const noexcept { return std::hash<std::string_view>{}(a_path); }
		};

		std::unordered_map<std::string, LooseFile, PathHash, std::equal_to<>> _loose;
		std::vector<std::unique_ptr<BsaArchive>> _archives;
		std::vector<std::string> _archiveErrors;
	};

	// The archives the game loads for a load order, lowest priority first: the INI lists (sResourceArchiveList and
	// sResourceArchiveList2, comma-separated names), then for each plugin "<name>.bsa" and "<name> - Textures.bsa"
	// where they exist in a data folder.
	[[nodiscard]] std::vector<std::filesystem::path> ArchivesForLoadOrder(
		const std::vector<std::filesystem::path>& a_dataRoots,
		const std::vector<std::string>& a_iniArchives,
		const std::vector<std::string>& a_pluginNames);
}
