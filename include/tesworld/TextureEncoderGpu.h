#pragma once

#include "tesworld/Gpu.h"
#include "tesworld/TextureEncoder.h"

#include <memory>

// Texture encoding on the GPU (tesworld::gpu): BC7 chains uploaded in frames on the context's thread, encoded with the
// terrain bake's BC7 kernel (BasicRenderer's mode 6), read back and wrapped as DirectXTex would. Other formats, and
// everything owed once the GPU fails, are encoded on the CPU.
namespace tesworld::texture
{
	// Throws gpu::UnsupportedError when its frames won't fit the adapter's memory.
	[[nodiscard]] std::unique_ptr<TextureEncoder> CreateGpuTextureEncoder(gpu::Context& a_context);
}