// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Vfs.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <zlib.h>

using namespace tesworld::vfs;

namespace
{
	struct TestFile
	{
		std::string folder;
		std::string name;
		std::string contents;
		bool compressed{ false };
	};

	template <class T>
	void Put(std::vector<std::uint8_t>& a_out, T a_value)
	{
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
		a_out.insert(a_out.end(), bytes, bytes + sizeof(T));
	}

	template <class T>
	void PutAt(std::vector<std::uint8_t>& a_out, std::size_t a_offset, T a_value)
	{
		std::memcpy(a_out.data() + a_offset, &a_value, sizeof(T));
	}

	// A BSA with one folder per file; the reader ignores hashes, so they are zero. Version 104 compresses with zlib, 105
	// with LZ4, so compressed files need 104 here.
	void WriteBsa(const std::filesystem::path& a_path, std::uint32_t a_version, const std::vector<TestFile>& a_files)
	{
		std::vector<std::uint8_t> out;
		std::uint32_t folderNames = 0, fileNames = 0;
		for (const auto& file : a_files) {
			folderNames += static_cast<std::uint32_t>(file.folder.size() + 1);
			fileNames += static_cast<std::uint32_t>(file.name.size() + 1);
		}
		const auto count = static_cast<std::uint32_t>(a_files.size());
		Put(out, 0x00415342u);
		Put(out, a_version);
		Put(out, 36u);
		Put(out, 0x3u);  // folder and file names, not compressed by default
		Put(out, count);
		Put(out, count);
		Put(out, folderNames);
		Put(out, fileNames);
		Put(out, 0u);
		for (std::uint32_t i = 0; i < count; ++i) {
			Put(out, std::uint64_t{ 0 });
			Put(out, 1u);
			if (a_version == 105u) {
				Put(out, 0u);
				Put(out, std::uint64_t{ 0 });
			} else {
				Put(out, 0u);
			}
		}
		std::vector<std::size_t> recordOffsets;
		for (const auto& file : a_files) {
			Put(out, static_cast<std::uint8_t>(file.folder.size() + 1));
			out.insert(out.end(), file.folder.begin(), file.folder.end());
			out.push_back(0);
			Put(out, std::uint64_t{ 0 });
			recordOffsets.push_back(out.size());
			Put(out, 0u);
			Put(out, 0u);
		}
		for (const auto& file : a_files) {
			out.insert(out.end(), file.name.begin(), file.name.end());
			out.push_back(0);
		}
		for (std::size_t i = 0; i < a_files.size(); ++i) {
			const auto& file = a_files[i];
			std::vector<std::uint8_t> payload(file.contents.begin(), file.contents.end());
			if (file.compressed) {
				uLongf size = compressBound(static_cast<uLong>(payload.size()));
				std::vector<std::uint8_t> packed(size);
				compress(packed.data(), &size, payload.data(), static_cast<uLong>(payload.size()));
				packed.resize(size);
				std::vector<std::uint8_t> withSize;
				Put(withSize, static_cast<std::uint32_t>(payload.size()));
				withSize.insert(withSize.end(), packed.begin(), packed.end());
				payload = std::move(withSize);
			}
			const auto sizeField = static_cast<std::uint32_t>(payload.size()) | (file.compressed ? 0x40000000u : 0u);
			PutAt(out, recordOffsets[i], sizeField);
			PutAt(out, recordOffsets[i] + 4, static_cast<std::uint32_t>(out.size()));
			out.insert(out.end(), payload.begin(), payload.end());
		}
		std::ofstream(a_path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
	}

	void WriteText(const std::filesystem::path& a_path, const std::string& a_text)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream(a_path, std::ios::binary) << a_text;
	}

	std::string AsText(const std::optional<std::vector<std::uint8_t>>& a_bytes)
	{
		assert(a_bytes.has_value());
		return std::string(a_bytes->begin(), a_bytes->end());
	}
}

int main()
{
	assert(NormalizePath("Data/Meshes//Foo/Bar.NIF") == "meshes\\foo\\bar.nif");
	assert(NormalizePath("\\textures\\a.dds ") == "textures\\a.dds");
	assert(NormalizePath("   ").empty());
	assert(NormalizePath(".BSA") == ".bsa");
	assert(FileName("meshes\\a\\b.nif") == "b.nif" && FileStem("meshes/a/b.nif") == "b" && FileStem("plain") == "plain");
	assert(NormalizePath(".\\Textures\\a.dds") == "textures\\a.dds");

	const auto root = std::filesystem::temp_directory_path() / "tesworld_vfs_test";
	std::filesystem::remove_all(root);
	const auto data = root / "Data";
	const auto mod = root / "mods" / "Better Rocks";
	std::filesystem::create_directories(data);

	const auto bsa = data / "Test.bsa";
	WriteBsa(bsa, 104u, {
		{ "textures", "a.dds", "from archive" },
		{ "meshes\\rocks", "rock01.nif", std::string(4000, 'r'), true },
	});
	std::string error;
	const auto archive = BsaArchive::Open(bsa, &error);
	assert(archive && error.empty());
	assert(archive->FileCount() == 2);
	assert(archive->Contains("meshes\\rocks\\rock01.nif"));
	assert(AsText(archive->Read("meshes\\rocks\\rock01.nif")) == std::string(4000, 'r'));
	assert(!archive->Read("missing.dds"));

	const auto sse = root / "Sse.bsa";
	WriteBsa(sse, 105u, { { "meshes", "a.nif", "version 105" } });
	const auto sseArchive = BsaArchive::Open(sse, &error);
	assert(sseArchive && AsText(sseArchive->Read("meshes\\a.nif")) == "version 105");

	WriteText(data / "textures" / "a.dds", "from data");
	WriteText(mod / "Textures" / "A.DDS", "from mod");
	WriteText(mod / "textures" / "b.dds", "only loose");

	const auto archives = ArchivesForLoadOrder({ data, mod }, { "Missing.bsa" }, { "Skyrim.esm", "Test.esp" });
	assert(archives.size() == 1 && archives.front().filename() == "Test.bsa");

	const FileSystem files(FileSystemOptions{ .looseRoots = { data, mod }, .archives = archives });
	assert(files.ArchiveErrors().empty());
	assert(AsText(files.Read("Textures/A.dds")) == "from mod");
	assert(files.Locate("textures\\a.dds")->origin == Origin::Loose);
	assert(AsText(files.Read("meshes\\rocks\\rock01.nif")) == std::string(4000, 'r'));
	assert(files.Locate("meshes\\rocks\\rock01.nif")->origin == Origin::Archive);
	assert(!files.Exists("textures\\c.dds"));
	const auto textures = files.Enumerate("Textures", ".DDS");
	assert(textures.size() == 2 && textures[0] == "textures\\a.dds" && textures[1] == "textures\\b.dds");

	// Stamps: the winning file's, loose or archived; a file written again (another size) is another stamp.
	const auto looseStamp = files.Stamp("textures\\a.dds");
	assert(looseStamp && looseStamp->origin == Origin::Loose && looseStamp->size == 8 && looseStamp->container == mod / "Textures" / "A.DDS");
	const auto archived = files.Stamp("meshes\\rocks\\rock01.nif");
	assert(archived && archived->origin == Origin::Archive && archived->container == bsa && archived->containerSize == std::filesystem::file_size(bsa));
	assert(archived != files.Stamp("textures\\b.dds") && !files.Stamp("textures\\c.dds"));
	WriteText(mod / "Textures" / "A.DDS", "from the mod, again");
	assert(FileSystem(FileSystemOptions{ .looseRoots = { data, mod } }).Stamp("textures\\a.dds") != looseStamp);

	const FileSystem broken(FileSystemOptions{ .archives = { data / "textures" / "a.dds" } });
	assert(broken.ArchiveErrors().size() == 1);

	std::filesystem::remove_all(root);
	std::cout << "tesworld vfs tests passed\n";
	return 0;
}
