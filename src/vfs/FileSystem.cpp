#include "tesworld/Vfs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <tuple>
#include <unordered_set>

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_for_each.h>

#include "tesworld/Platform.h"
#include "tesworld/Profiling.h"

namespace tesworld::vfs
{
	namespace
	{
		struct IndexedFile
		{
			std::filesystem::path path;
			std::uint64_t size{ 0 };
			std::int64_t time{ 0 };
		};
		using LooseIndex = std::vector<std::pair<std::string, IndexedFile>>;

		// A directory to list: its root, and its path relative to the root as the game spells it ('/'-separated, "" at the
		// root).
		struct Directory
		{
			std::size_t root{ 0 };
			std::filesystem::path path;
			std::string relative;
		};

		// Every root's files, each directory listed as a task of its own: a mod with a hundred thousand loose files no
		// longer holds the rest up. Within a root a path is listed once; sorted, so which of two spellings of one path
		// wins doesn't depend on timing.
		std::vector<LooseIndex> IndexRoots(const std::vector<std::filesystem::path>& a_roots)
		{
			std::vector<LooseIndex> indexes(a_roots.size());
			std::vector<std::mutex> mutexes(a_roots.size());
			std::vector<Directory> roots;
			for (std::size_t i = 0; i < a_roots.size(); ++i) {
				std::error_code ec;
				if (std::filesystem::is_directory(a_roots[i], ec)) roots.push_back({ i, a_roots[i], {} });
			}
			oneapi::tbb::parallel_for_each(roots.begin(), roots.end(), [&](const Directory& a_directory, oneapi::tbb::feeder<Directory>& a_feeder) {
				TW_ZONE("index directory");
				LooseIndex files;
				std::error_code ec;
				const auto options = std::filesystem::directory_options::skip_permission_denied;
				for (auto it = std::filesystem::directory_iterator(a_directory.path, options, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
					const auto name = it->path().filename().u8string();
					auto relative = a_directory.relative;
					if (!relative.empty()) relative.push_back('/');
					relative.append(name.begin(), name.end());
					if (it->is_directory(ec) && !it->is_symlink(ec)) {
						a_feeder.add(Directory{ a_directory.root, it->path(), std::move(relative) });
						continue;
					}
					if (!it->is_regular_file(ec)) {
						ec.clear();
						continue;
					}
					// Plugins, archives and MO2's metadata are not assets.
					auto normalized = NormalizePath(relative);
					if (normalized.empty() || normalized == "meta.ini" || normalized.ends_with(".bsa") || normalized.ends_with(".ba2") ||
						normalized.ends_with(".esp") || normalized.ends_with(".esm") || normalized.ends_with(".esl")) {
						continue;
					}
					// The directory listing's size and time: no more calls to the file system.
					const auto size = it->file_size(ec);
					const auto time = ec ? std::filesystem::file_time_type{} : it->last_write_time(ec);
					files.emplace_back(std::move(normalized), IndexedFile{ .path = it->path(), .size = ec ? 0 : size, .time = time.time_since_epoch().count() });
					ec.clear();
				}
				if (files.empty()) return;
				std::scoped_lock lock(mutexes[a_directory.root]);
				auto& index = indexes[a_directory.root];
				index.insert(index.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
			});
			oneapi::tbb::parallel_for(std::size_t{ 0 }, indexes.size(), [&](std::size_t i) {
				std::ranges::sort(indexes[i], [](const auto& a, const auto& b) { return std::tie(a.first, a.second.path) < std::tie(b.first, b.second.path); });
			});
			return indexes;
		}

		std::string LowerExtension(std::string_view a_extension)
		{
			std::string result(a_extension);
			std::ranges::transform(result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return result;
		}
	}

	FileSystem::FileSystem(FileSystemOptions a_options)
	{
		auto indexes = IndexRoots(a_options.looseRoots);
		// Later roots have priority, so they overwrite earlier entries.
		{
			TW_ZONE("merge loose roots");
			for (auto& index : indexes) {
				for (auto& [path, file] : index) {
					_loose.insert_or_assign(std::move(path), LooseFile{ .path = std::move(file.path), .size = file.size, .time = file.time });
				}
			}
		}

		std::vector<std::unique_ptr<BsaArchive>> archives(a_options.archives.size());
		std::vector<std::string> errors(a_options.archives.size());
		oneapi::tbb::parallel_for(std::size_t{ 0 }, a_options.archives.size(), [&](std::size_t i) {
			TW_ZONE("open archive");
			archives[i] = BsaArchive::Open(a_options.archives[i], &errors[i]);
		});
		for (std::size_t i = 0; i < archives.size(); ++i) {
			if (archives[i]) {
				_archives.push_back(std::move(archives[i]));
			} else {
				_archiveErrors.push_back(platform::PathToUtf8(a_options.archives[i]) + ": " + errors[i]);
			}
		}
	}

	FileSystem::~FileSystem() = default;

	namespace
	{
		// A path normalized into the thread's buffer, for a lookup: no string made for each.
		const std::string& Normalized(std::string_view a_path)
		{
			thread_local std::string buffer;
			NormalizePath(a_path, buffer);
			return buffer;
		}
	}

	std::optional<Located> FileSystem::Locate(std::string_view a_path) const
	{
		const auto& normalized = Normalized(a_path);
		if (normalized.empty()) {
			return std::nullopt;
		}
		if (const auto it = _loose.find(normalized); it != _loose.end()) {
			return Located{ Origin::Loose, it->second.path };
		}
		for (auto it = _archives.rbegin(); it != _archives.rend(); ++it) {
			if ((*it)->Contains(normalized)) {
				return Located{ Origin::Archive, (*it)->Path() };
			}
		}
		return std::nullopt;
	}

	bool FileSystem::Exists(std::string_view a_path) const
	{
		return Locate(a_path).has_value();
	}

	std::optional<std::vector<std::uint8_t>> FileSystem::Read(std::string_view a_path) const
	{
		TW_ZONE("FileSystem::Read");
		const auto& normalized = Normalized(a_path);
		if (normalized.empty()) {
			return std::nullopt;
		}
		if (const auto it = _loose.find(normalized); it != _loose.end()) {
			try {
				return platform::ReadFileBytes(it->second.path);
			} catch (const std::exception&) {
				return std::nullopt;
			}
		}
		for (auto it = _archives.rbegin(); it != _archives.rend(); ++it) {
			if ((*it)->Contains(normalized)) {
				return (*it)->Read(normalized);
			}
		}
		return std::nullopt;
	}

	std::optional<std::vector<std::uint8_t>> FileSystem::ReadRange(std::string_view a_path, std::uint64_t a_offset, std::uint64_t a_length) const
	{
		TW_ZONE("FileSystem::ReadRange");
		const auto& normalized = Normalized(a_path);
		if (normalized.empty()) {
			return std::nullopt;
		}
		if (const auto it = _loose.find(normalized); it != _loose.end()) {
			std::ifstream input(it->second.path, std::ios::binary | std::ios::ate);
			if (!input) return std::nullopt;
			const auto size = static_cast<std::uint64_t>(input.tellg());
			const auto from = (std::min)(a_offset, size);
			std::vector<std::uint8_t> bytes((std::min)(a_length, size - from));
			input.seekg(static_cast<std::streamoff>(from), std::ios::beg);
			if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
			return bytes;
		}
		for (auto it = _archives.rbegin(); it != _archives.rend(); ++it) {
			if ((*it)->Contains(normalized)) {
				return (*it)->ReadRange(normalized, a_offset, a_length);
			}
		}
		return std::nullopt;
	}

	std::optional<FileStamp> FileSystem::Stamp(std::string_view a_path) const
	{
		const auto& normalized = Normalized(a_path);
		if (normalized.empty()) {
			return std::nullopt;
		}
		if (const auto it = _loose.find(normalized); it != _loose.end()) {
			return FileStamp{ .origin = Origin::Loose, .container = it->second.path, .size = it->second.size, .time = it->second.time };
		}
		for (auto it = _archives.rbegin(); it != _archives.rend(); ++it) {
			if (auto stamp = (*it)->Stamp(normalized)) {
				return stamp;
			}
		}
		return std::nullopt;
	}

	std::vector<std::string> FileSystem::Enumerate(std::string_view a_folder, std::string_view a_extension) const
	{
		auto prefix = NormalizePath(a_folder);
		if (!prefix.empty() && !prefix.ends_with('\\')) {
			prefix.push_back('\\');
		}
		const auto extension = LowerExtension(a_extension);
		const auto matches = [&](std::string_view a_path) {
			return a_path.starts_with(prefix) && (extension.empty() || a_path.ends_with(extension));
		};

		std::vector<std::string_view> found;
		for (const auto& [path, _] : _loose) {
			if (matches(path)) found.push_back(path);
		}
		for (const auto& archive : _archives) {
			for (const auto path : archive->Paths()) {
				if (matches(path)) found.push_back(path);
			}
		}
		std::ranges::sort(found);
		const auto [last, end] = std::ranges::unique(found);
		found.erase(last, end);
		return std::vector<std::string>(found.begin(), found.end());
	}

	std::vector<std::filesystem::path> ArchivesForLoadOrder(
		const std::vector<std::filesystem::path>& a_dataRoots,
		const std::vector<std::string>& a_iniArchives,
		const std::vector<std::string>& a_pluginNames)
	{
		// The archives at the top of each data root; the last root holding one wins, as for loose files.
		std::unordered_map<std::string, std::filesystem::path> available;
		for (const auto& root : a_dataRoots) {
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
				auto name = NormalizePath(platform::PathToUtf8(entry.path().filename()));
				if (name.ends_with(".bsa")) {
					available.insert_or_assign(std::move(name), entry.path());
				}
			}
		}
		const auto find = [&](const std::string& a_name) -> std::optional<std::filesystem::path> {
			const auto it = available.find(NormalizePath(a_name));
			return it == available.end() ? std::nullopt : std::optional{ it->second };
		};

		std::vector<std::filesystem::path> result;
		std::unordered_set<std::string> added;
		const auto add = [&](const std::string& a_name) {
			const auto key = NormalizePath(a_name);
			if (key.empty() || added.contains(key)) {
				return;
			}
			if (auto path = find(a_name)) {
				added.insert(key);
				result.push_back(std::move(*path));
			}
		};

		for (const auto& name : a_iniArchives) {
			add(name);
		}
		for (const auto& plugin : a_pluginNames) {
			const auto stem = std::string(FileStem(plugin));
			add(stem + ".bsa");
			add(stem + " - Textures.bsa");
		}
		return result;
	}
}
