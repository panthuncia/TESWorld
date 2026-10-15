#pragma once

#include "tesworld/Texture.h"

#include <array>

// SARP's histogram-preserving stochastic texturing (BasicRenderer's TextureProcessingManager), on the CPU: a texture is
// remapped to a Gaussian histogram, so blending three offset samples keeps its contrast, and an inverse lookup table per
// mip level maps the blend back to the texture's own values. Colour textures are first moved to decorrelated axes.
namespace tesworld::texture
{
	enum class StochasticMode
	{
		DecorrelatedColor,
		NormalXY,
		Scalar
	};

	struct StochasticTexture
	{
		StochasticMode mode{ StochasticMode::DecorrelatedColor };
		// The Gaussianized texture, mipmapped by box filter; channels as the mode uses them (RGB, RG or R).
		MipChain gaussian;
		// lutWidth x lutHeight: a row per source mip level, a column per Gaussian value.
		Image inverseLut;
		std::uint32_t lutHeight{ 0 };
		// Decorrelated colour axes: colour = origin + v0 * c.r + v1 * c.g + v2 * c.b.
		Vec3 colorSpaceOrigin{};
		Vec3 colorSpaceVector0{ 1, 0, 0 };
		Vec3 colorSpaceVector1{ 0, 1, 0 };
		Vec3 colorSpaceVector2{ 0, 0, 1 };
	};

	// From the largest level of a source chain. Normal maps should already be in Direct3D's convention.
	[[nodiscard]] StochasticTexture BuildStochasticTexture(const MipChain& a_source, StochasticMode a_mode, std::uint32_t a_lutWidth = 256);

	[[nodiscard]] double InverseNormalCdf(double a_probability);
}
