#pragma once

#include "tesworld/Gpu.h"
#include "tesworld/TerrainBake.h"

#include <memory>

// The terrain bake on the GPU (tesworld::gpu): the same images as the CPU baker's (TerrainBake.h) within a step or two of
// a byte (the GPU's division, square roots and logarithms round differently), with mips and BC7 made there too. Requests
// are batched into frames on the context's thread, and called back from TBB tasks as frames complete. If the GPU fails
// part way, what it owes is baked on the CPU instead.
namespace tesworld::texture
{
	// Uploads the worldspace's landscape lattice. Throws gpu::UnsupportedError when it won't fit the adapter's memory.
	[[nodiscard]] std::unique_ptr<TerrainBaker> CreateGpuTerrainBaker(gpu::Context& a_context, const WorldTerrain& a_terrain, TerrainBakeSettings a_settings);
}
