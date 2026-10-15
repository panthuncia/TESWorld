#pragma once

#include "tesworld/Nif.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tesworld::nif::detail
{
	// WriteNif as nifly writes it: the scene graph built from nifly's blocks and saved. Slower and allocates per
	// vertex; the tests hold WriteNif to its bytes.
	[[nodiscard]] bool WriteNifWithNifly(const OutNode& a_root, std::vector<std::uint8_t>& a_output, std::string* a_error = nullptr);
}
