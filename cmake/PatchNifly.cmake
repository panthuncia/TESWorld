# Run as nifly's PATCH_COMMAND (in its source folder). Each patch is idempotent, and replaces an earlier form of itself.
#
# BoundingSphere copies every vertex into a std::list of std::vectors for Miniball: two allocations a vertex, most of the
# time a LOD block takes to write. Miniball reads the points through an accessor and keeps its own list of them, so a
# contiguous copy gives the same sphere: here a pointer to each point's coordinates, which Miniball's accessor takes as
# they are on every standard library (an array's iterator is a pointer on some).
#
# Reading a NIF trims its texture paths with regular expressions compiled again for every path, and parses its version
# with one compiled for every file: millions of allocations in a LOD run, and most of the time a model takes to read.
# They're compiled once instead, as constants (a const std::regex is safe to use from many threads), the same matching.

# Replaces a_before with a_after in a_file, or a_earlier (an earlier form of the patch) with a_after; nothing if a_after
# is there already.
function(nifly_patch a_file a_before a_after a_earlier)
	file(READ "${a_file}" source)
	# A checkout may have Windows line endings.
	string(REPLACE "\r\n" "\n" source "${source}")
	string(FIND "${source}" "${a_after}" patched)
	if(NOT patched EQUAL -1)
		return()
	endif()
	if(NOT a_earlier STREQUAL "")
		string(FIND "${source}" "${a_earlier}" found)
		if(NOT found EQUAL -1)
			string(REPLACE "${a_earlier}" "${a_after}" source "${source}")
			file(WRITE "${a_file}" "${source}")
			return()
		endif()
	endif()
	string(FIND "${source}" "${a_before}" found)
	if(found EQUAL -1)
		message(FATAL_ERROR "PatchNifly: ${a_file} isn't what the patch expects (nifly's pin changed?)")
	endif()
	string(REPLACE "${a_before}" "${a_after}" source "${source}")
	file(WRITE "${a_file}" "${source}")
endfunction()

nifly_patch("src/Object3d.cpp" [=[#include <Miniball.hpp>
]=] [=[#include <Miniball.hpp>
#include <array>
]=] "")
nifly_patch("src/Object3d.cpp" [=[	std::list<std::vector<float>> lp;
	for (auto vertice : vertices) {
		lp.push_back({vertice.x, vertice.y, vertice.z});
	}

	Miniball::Miniball<Miniball::CoordAccessor<std::list<std::vector<float>>::const_iterator,
											   std::vector<float>::const_iterator>>
		mb(3, lp.begin(), lp.end());]=] [=[	std::vector<std::array<float, 3>> lp;
	lp.reserve(vertices.size());
	for (const auto& vertice : vertices) {
		lp.push_back({vertice.x, vertice.y, vertice.z});
	}
	std::vector<const float*> points;
	points.reserve(lp.size());
	for (const auto& point : lp) {
		points.push_back(point.data());
	}

	Miniball::Miniball<Miniball::CoordAccessor<std::vector<const float*>::const_iterator, const float*>>
		mb(3, points.begin(), points.end());]=] [=[	std::vector<std::array<float, 3>> lp;
	lp.reserve(vertices.size());
	for (const auto& vertice : vertices) {
		lp.push_back({vertice.x, vertice.y, vertice.z});
	}

	Miniball::Miniball<Miniball::CoordAccessor<std::vector<std::array<float, 3>>::const_iterator,
											   std::array<float, 3>::const_iterator>>
		mb(3, lp.begin(), lp.end());]=])

nifly_patch("src/NifFile.cpp" [=[void NifFile::TrimTexturePaths() {
	auto fTrimPath = [&hdr = hdr, &isTerrain = isTerrain](std::string& tex) -> std::string& {
		if (tex.empty())
			return tex;

		// Trim whitespace characters (including newlines)
		trim_whitespace(tex);

		if (tex.empty())
			return tex;

		// Replace multiple slashes or forward slashes with one backslash
		tex = std::regex_replace(tex, std::regex("/+|\\\\+"), "\\");

		// Search for the first occurrence of "\textures\" (only if "textures\" isn't at the start)
		std::smatch match;
		std::regex pattern(R"(^(?!textures\\).*?\\textures\\)", std::regex_constants::icase);
	
		if (std::regex_search(tex, match, pattern))
			tex = tex.substr(match[0].length()); // Remove matched string

		// Remove all backslashes from the front
		tex = std::regex_replace(tex, std::regex("^\\\\+"), "");

		if (!hdr.GetVersion().IsMW() && !hdr.GetVersion().IsOB() && !hdr.GetVersion().IsSpecial()
			&& is_relative_path(tex)) {
			// If the path doesn't start with "textures\", add it to the front
			tex = std::regex_replace(tex,
									 std::regex("^(?!^textures\\\\)", std::regex_constants::icase),
									 "textures\\");
		}

		// If the path doesn't start with "Data\", add it to the front
		if (isTerrain && is_relative_path(tex)) {
			tex = std::regex_replace(tex, std::regex("^(?!^Data\\\\)", std::regex_constants::icase), "Data\\");
		}
]=] [=[void NifFile::TrimTexturePaths() {
	static const std::regex slashes("/+|\\\\+");
	static const std::regex texturesWithin(R"(^(?!textures\\).*?\\textures\\)", std::regex_constants::icase);
	static const std::regex leadingSlashes("^\\\\+");
	static const std::regex notTextures("^(?!^textures\\\\)", std::regex_constants::icase);
	static const std::regex notData("^(?!^Data\\\\)", std::regex_constants::icase);
	auto fTrimPath = [&hdr = hdr, &isTerrain = isTerrain](std::string& tex) -> std::string& {
		if (tex.empty())
			return tex;

		// Trim whitespace characters (including newlines)
		trim_whitespace(tex);

		if (tex.empty())
			return tex;

		// Replace multiple slashes or forward slashes with one backslash
		tex = std::regex_replace(tex, slashes, "\\");

		// Search for the first occurrence of "\textures\" (only if "textures\" isn't at the start)
		std::smatch match;
		const std::regex& pattern = texturesWithin;
	
		if (std::regex_search(tex, match, pattern))
			tex = tex.substr(match[0].length()); // Remove matched string

		// Remove all backslashes from the front
		tex = std::regex_replace(tex, leadingSlashes, "");

		if (!hdr.GetVersion().IsMW() && !hdr.GetVersion().IsOB() && !hdr.GetVersion().IsSpecial()
			&& is_relative_path(tex)) {
			// If the path doesn't start with "textures\", add it to the front
			tex = std::regex_replace(tex,
									 notTextures,
									 "textures\\");
		}

		// If the path doesn't start with "Data\", add it to the front
		if (isTerrain && is_relative_path(tex)) {
			tex = std::regex_replace(tex, notData, "Data\\");
		}
]=] "")
nifly_patch("src/BasicTypes.cpp" [=[		std::regex reg("25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9]");
]=] [=[		static const std::regex reg("25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9]");
]=] "")
