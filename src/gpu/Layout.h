#pragma once

// shaders/Layout.hlsli compiled as C++: the structs the kernels read, as the C++ that fills them sees them.

#include <cstdint>

namespace tesworld::gpu::layout
{
	using uint = std::uint32_t;

	namespace
	{
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "shaders/Layout.hlsli"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
	}

	static_assert(sizeof(BakeConstants) == 64 && sizeof(MaterialHeader) == 80 && sizeof(RegionGpu) == 16 && sizeof(LayerGpu) == 48 &&
	              sizeof(TextureGpu) == 16 && sizeof(LevelGpu) == 16 && sizeof(StochasticGpu) == 64 && sizeof(WorldHeader) == 32 &&
	              sizeof(ViewFrameHeader) == 32 && sizeof(ViewDrawGpu) == 144 && sizeof(ViewResolveConstants) == 96,
		"the kernels index these by their sizes");
}
