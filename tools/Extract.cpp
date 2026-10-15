// tesworld-extract <data folder> <output folder> <game path>...
// Copies files out of a Data folder as the game sees them: loose files over the BSAs there (in name order).

#include "tesworld/Vfs.h"

#include <algorithm>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
	if (argc < 4) {
		std::cerr << "usage: tesworld-extract <data folder> <output folder> <game path>...\n";
		return 2;
	}
	const std::filesystem::path data = argv[1];
	const std::filesystem::path output = argv[2];
	std::vector<std::filesystem::path> archives;
	for (const auto& entry : std::filesystem::directory_iterator(data)) {
		if (tesworld::vfs::NormalizePath(entry.path().extension().string()) == ".bsa") {
			archives.push_back(entry.path());
		}
	}
	std::ranges::sort(archives);
	const tesworld::vfs::FileSystem files({ .looseRoots = { data }, .archives = archives });
	for (const auto& error : files.ArchiveErrors()) {
		std::cerr << "skipped " << error << '\n';
	}

	int missing = 0;
	for (int i = 3; i < argc; ++i) {
		const auto bytes = files.Read(argv[i]);
		if (!bytes) {
			std::cerr << "not found: " << argv[i] << '\n';
			++missing;
			continue;
		}
		const auto target = output / std::string(tesworld::vfs::FileName(tesworld::vfs::NormalizePath(argv[i])));
		std::ofstream(target, std::ios::binary).write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
		std::cout << argv[i] << " -> " << target.string() << " (" << bytes->size() << " bytes)\n";
	}
	return missing == 0 ? 0 : 1;
}
