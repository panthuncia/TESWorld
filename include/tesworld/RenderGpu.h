#pragma once

#include "tesworld/Gpu.h"
#include "tesworld/Render.h"

#include <memory>

// Views rendered on the GPU (tesworld::gpu): requests taken in frames on the context's thread, each view a rectangle of
// a large target, drawn by the hardware's rasterizer with CpuRenderer's shading and texture filtering in shader
// arithmetic, box-filtered by a kernel and read back. A view too large for a frame's target, and everything owed once
// the GPU fails, is rendered on the CPU.
namespace tesworld::render
{
	// Throws gpu::UnsupportedError when its frames won't fit the adapter's memory.
	[[nodiscard]] std::unique_ptr<ViewRenderer> CreateGpuViewRenderer(gpu::Context& a_context);
}