#include "tesworld/Stochastic.h"

#include "tesworld/Profiling.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tesworld::texture
{
	namespace
	{
		using Rgba = std::array<float, 4>;
		using Axis = std::array<float, 3>;

		std::uint8_t Quantize(float a_value)
		{
			return static_cast<std::uint8_t>(std::clamp(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f), 0L, 255L));
		}

		float GaussianizedValue(float a_cdf)
		{
			return std::clamp(0.5f + static_cast<float>(InverseNormalCdf(a_cdf)) * (1.0f / 6.0f), 0.0f, 1.0f);
		}

		float Dot3(const Axis& a, const Axis& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		Axis Cross3(const Axis& a, const Axis& b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
		Axis Normalize3(Axis a)
		{
			const float length = std::sqrt(Dot3(a, a));
			return length > 1.0e-8f ? Axis{ a[0] / length, a[1] / length, a[2] / length } : Axis{ 1.0f, 0.0f, 0.0f };
		}

		std::array<Axis, 3> JacobiEigenvectors3x3(std::array<Axis, 3> a)
		{
			std::array<Axis, 3> v = { Axis{ 1, 0, 0 }, Axis{ 0, 1, 0 }, Axis{ 0, 0, 1 } };
			for (std::uint32_t iteration = 0; iteration < 12u; ++iteration) {
				std::uint32_t p = 0, q = 1;
				float maxOffDiagonal = std::abs(a[0][1]);
				if (std::abs(a[0][2]) > maxOffDiagonal) { p = 0; q = 2; maxOffDiagonal = std::abs(a[0][2]); }
				if (std::abs(a[1][2]) > maxOffDiagonal) { p = 1; q = 2; maxOffDiagonal = std::abs(a[1][2]); }
				if (maxOffDiagonal < 1.0e-8f) break;

				const float theta = 0.5f * std::atan2(2.0f * a[p][q], a[q][q] - a[p][p]);
				const float c = std::cos(theta);
				const float s = std::sin(theta);
				for (std::uint32_t k = 0; k < 3u; ++k) {
					const float akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (std::uint32_t k = 0; k < 3u; ++k) {
					const float apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (std::uint32_t k = 0; k < 3u; ++k) {
					const float vkp = v[k][p], vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
			std::array<std::uint32_t, 3> order = { 0u, 1u, 2u };
			std::ranges::sort(order, [&](std::uint32_t lhs, std::uint32_t rhs) { return a[lhs][lhs] > a[rhs][rhs]; });
			std::array<Axis, 3> axes{};
			for (std::uint32_t out = 0; out < 3u; ++out) {
				axes[out] = Normalize3({ v[0][order[out]], v[1][order[out]], v[2][order[out]] });
			}
			if (Dot3(Cross3(axes[0], axes[1]), axes[2]) < 0.0f) {
				axes[2] = { -axes[2][0], -axes[2][1], -axes[2][2] };
			}
			return axes;
		}

		// Moves colours onto their principal axes, each stretched to 0..1, and records how to move back.
		void Decorrelate(std::vector<Rgba>& a_pixels, StochasticTexture& a_result)
		{
			if (a_pixels.empty()) return;
			Axis mean{ 0, 0, 0 };
			for (const auto& pixel : a_pixels) {
				for (int c = 0; c < 3; ++c) mean[c] += std::clamp(pixel[c], 0.0f, 1.0f);
			}
			const float invCount = 1.0f / static_cast<float>(a_pixels.size());
			for (auto& m : mean) m *= invCount;
			std::array<Axis, 3> covariance{};
			for (const auto& pixel : a_pixels) {
				const Axis d{ std::clamp(pixel[0], 0.0f, 1.0f) - mean[0], std::clamp(pixel[1], 0.0f, 1.0f) - mean[1], std::clamp(pixel[2], 0.0f, 1.0f) - mean[2] };
				for (int row = 0; row < 3; ++row) {
					for (int col = 0; col < 3; ++col) covariance[row][col] += d[row] * d[col] * invCount;
				}
			}
			const auto axes = JacobiEigenvectors3x3(covariance);
			Axis minimum{ (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)() };
			Axis maximum{ -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)() };
			const auto rgbOf = [](const Rgba& a_pixel) { return Axis{ std::clamp(a_pixel[0], 0.0f, 1.0f), std::clamp(a_pixel[1], 0.0f, 1.0f), std::clamp(a_pixel[2], 0.0f, 1.0f) }; };
			for (const auto& pixel : a_pixels) {
				const auto rgb = rgbOf(pixel);
				for (int axis = 0; axis < 3; ++axis) {
					const float projection = Dot3(axes[axis], rgb);
					minimum[axis] = (std::min)(minimum[axis], projection);
					maximum[axis] = (std::max)(maximum[axis], projection);
				}
			}
			for (auto& pixel : a_pixels) {
				const auto rgb = rgbOf(pixel);
				for (int axis = 0; axis < 3; ++axis) {
					const float range = maximum[axis] - minimum[axis];
					pixel[axis] = range > 1.0e-6f ? std::clamp((Dot3(axes[axis], rgb) - minimum[axis]) / range, 0.0f, 1.0f) : 0.5f;
				}
			}
			const auto scaled = [&](int a_axis) {
				const float range = maximum[a_axis] - minimum[a_axis];
				return Vec3{ axes[a_axis][0] * range, axes[a_axis][1] * range, axes[a_axis][2] * range };
			};
			a_result.colorSpaceVector0 = scaled(0);
			a_result.colorSpaceVector1 = scaled(1);
			a_result.colorSpaceVector2 = scaled(2);
			a_result.colorSpaceOrigin = {
				axes[0][0] * minimum[0] + axes[1][0] * minimum[1] + axes[2][0] * minimum[2],
				axes[0][1] * minimum[0] + axes[1][1] * minimum[1] + axes[2][1] * minimum[2],
				axes[0][2] * minimum[0] + axes[1][2] * minimum[1] + axes[2][2] * minimum[2],
			};
		}

		std::vector<std::uint8_t> Gaussianize(const std::vector<Rgba>& a_pixels, std::uint32_t a_channels)
		{
			const auto count = a_pixels.size();
			std::vector<std::uint8_t> result(count * 4, 0);
			for (std::uint32_t channel = 0; channel < a_channels; ++channel) {
				std::vector<std::pair<float, std::size_t>> sorted;
				sorted.reserve(count);
				for (std::size_t i = 0; i < count; ++i) sorted.emplace_back(std::clamp(a_pixels[i][channel], 0.0f, 1.0f), i);
				std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				for (std::size_t rank = 0; rank < count; ++rank) {
					const float cdf = (static_cast<float>(rank) + 0.5f) / static_cast<float>((std::max)(std::size_t{ 1 }, count));
					result[sorted[rank].second * 4 + channel] = Quantize(GaussianizedValue(cdf));
				}
			}
			for (std::size_t i = 0; i < count; ++i) {
				result[i * 4 + 3] = Quantize(a_pixels[i][3]);
			}
			return result;
		}

		float AverageWindowVariance(const Image& a_gaussian, std::uint32_t a_channel, std::uint32_t a_windowSide)
		{
			double varianceSum = 0.0;
			std::uint32_t blocks = 0;
			for (std::uint32_t y0 = 0; y0 < a_gaussian.height; y0 += a_windowSide) {
				for (std::uint32_t x0 = 0; x0 < a_gaussian.width; x0 += a_windowSide) {
					const auto y1 = (std::min)(a_gaussian.height, y0 + a_windowSide);
					const auto x1 = (std::min)(a_gaussian.width, x0 + a_windowSide);
					double sum = 0.0, sumSquares = 0.0;
					std::uint32_t n = 0;
					for (auto y = y0; y < y1; ++y) {
						for (auto x = x0; x < x1; ++x) {
							const double value = a_gaussian.rgba[(static_cast<std::size_t>(y) * a_gaussian.width + x) * 4 + a_channel] / 255.0;
							sum += value;
							sumSquares += value * value;
							++n;
						}
					}
					if (n > 1) {
						const double mean = sum / n;
						varianceSum += sumSquares / n - mean * mean;
						++blocks;
					}
				}
			}
			return blocks == 0 ? 0.0f : static_cast<float>(varianceSum / blocks);
		}

		// Row 0 maps a Gaussian value back to the source's value at that quantile; lower rows are filtered by the
		// variance a box of that mip's footprint averages away, as the GPU's mips of the Gaussian texture lose it.
		Image BuildInverseLut(const std::vector<Rgba>& a_pixels, std::uint32_t a_channels, std::uint32_t a_width, std::uint32_t a_height, const Image& a_gaussian)
		{
			Image lut(a_width, a_height);
			for (std::uint32_t channel = 0; channel < a_channels; ++channel) {
				std::vector<float> sorted;
				sorted.reserve(a_pixels.size());
				for (const auto& pixel : a_pixels) sorted.push_back(std::clamp(pixel[channel], 0.0f, 1.0f));
				std::ranges::sort(sorted);
				std::vector<float> base(a_width, 0.0f);
				for (std::uint32_t x = 0; x < a_width; ++x) {
					const float g = (static_cast<float>(x) + 0.5f) / static_cast<float>(a_width);
					const float cdf = 0.5f * (1.0f + std::erf((g - 0.5f) * 6.0f / std::sqrt(2.0f)));
					const auto index = (std::min)(sorted.size() - 1, static_cast<std::size_t>(std::clamp(cdf, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1)));
					base[x] = sorted[index];
				}
				for (std::uint32_t y = 0; y < a_height; ++y) {
					const float variance = y == 0 ? 0.0f : AverageWindowVariance(a_gaussian, channel, 1u << (std::min)(y, 12u));
					for (std::uint32_t x = 0; x < a_width; ++x) {
						float filtered = base[x];
						if (variance > 1.0e-8f) {
							const float center = (static_cast<float>(x) + 0.5f) / static_cast<float>(a_width);
							const int radius = static_cast<int>((std::min)(static_cast<float>(a_width), std::ceil(std::sqrt(variance) * 4.0f * a_width)));
							double weighted = 0.0, weightSum = 0.0;
							for (int dx = -radius; dx <= radius; ++dx) {
								const int sx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(a_width) - 1);
								const float d = (static_cast<float>(sx) + 0.5f) / static_cast<float>(a_width) - center;
								const float w = std::exp(-(d * d) / (2.0f * variance));
								weighted += static_cast<double>(base[static_cast<std::size_t>(sx)]) * w;
								weightSum += w;
							}
							if (weightSum > 0.0) filtered = static_cast<float>(weighted / weightSum);
						}
						lut.rgba[(static_cast<std::size_t>(y) * a_width + x) * 4 + channel] = Quantize(filtered);
					}
				}
			}
			for (std::size_t i = 3; i < lut.rgba.size(); i += 4) lut.rgba[i] = 255;
			return lut;
		}
	}

	double InverseNormalCdf(double p)
	{
		p = std::clamp(p, 1.0e-6, 1.0 - 1.0e-6);
		static constexpr double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
		static constexpr double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
		static constexpr double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
		static constexpr double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00 };
		static constexpr double pLow = 0.02425;
		static constexpr double pHigh = 1.0 - pLow;
		if (p < pLow) {
			const double q = std::sqrt(-2.0 * std::log(p));
			return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
		}
		if (p > pHigh) {
			const double q = std::sqrt(-2.0 * std::log(1.0 - p));
			return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
		}
		const double q = p - 0.5;
		const double r = q * q;
		return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
	}

	StochasticTexture BuildStochasticTexture(const MipChain& a_source, StochasticMode a_mode, std::uint32_t a_lutWidth)
	{
		TW_ZONE("BuildStochasticTexture");
		StochasticTexture result;
		result.mode = a_mode;
		if (a_source.Empty()) {
			return result;
		}
		const auto& top = a_source.levels.front();
		std::vector<Rgba> pixels(static_cast<std::size_t>(top.width) * top.height);
		for (std::uint32_t y = 0; y < top.height; ++y) {
			for (std::uint32_t x = 0; x < top.width; ++x) {
				const auto t = top.Texel(x, y);
				pixels[static_cast<std::size_t>(y) * top.width + x] = { t.x, t.y, t.z, t.w };
			}
		}
		if (a_mode == StochasticMode::DecorrelatedColor) {
			Decorrelate(pixels, result);
		}
		const std::uint32_t channels = a_mode == StochasticMode::NormalXY ? 2u : (a_mode == StochasticMode::Scalar ? 1u : 3u);
		Image gaussian(top.width, top.height);
		gaussian.rgba = Gaussianize(pixels, channels);
		result.lutHeight = static_cast<std::uint32_t>(a_source.levels.size());
		result.inverseLut = BuildInverseLut(pixels, channels, (std::max)(16u, a_lutWidth), result.lutHeight, gaussian);
		result.gaussian = BuildMips(std::move(gaussian));
		return result;
	}
}
