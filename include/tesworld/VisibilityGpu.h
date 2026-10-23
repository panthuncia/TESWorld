#pragma once

#include "tesworld/Gpu.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Which squares of a heightfield can be seen from points above it, traced with hardware ray tracing (tesworld::gpu):
// the ground is triangulated into one acceleration structure, and rays from each viewpoint to points on a square's top
// decide it; only the ground blocks them. Groups are traced in frames on the context's thread, as its other clients'
// work is. What oculory-lod's occlusion data is made from.
namespace tesworld::gpu
{
	// Heights at a grid's points, `spacing` units apart, joined by triangles: heights[x * rows + y] is the ground at
	// (x * spacing, y * spacing), x east and y north.
	struct VisibilityGround
	{
		std::uint32_t columns{ 0 };
		std::uint32_t rows{ 0 };
		float spacing{ 0.0f };
		std::vector<float> heights;
	};

	// The squares seen, each `size` units across from (0, 0) as the ground's, its top at tops[x * rows + y], sampled by
	// samples x samples points evenly over it.
	struct VisibilityTargets
	{
		std::uint32_t columns{ 0 };
		std::uint32_t rows{ 0 };
		float size{ 0.0f };
		std::vector<float> tops;
		std::uint32_t samples{ 4 };
	};

	// Viewpoints whose views are joined: a target is visible when any of them sees it.
	struct VisibilityGroup
	{
		std::vector<std::array<float, 3>> viewpoints;
	};

	struct VisibilityResult
	{
		// A flag per target, in tops' order; empty when the GPU failed, error saying why: the caller works it out itself.
		std::vector<std::uint8_t> visible;
		std::string error;
	};

	class VisibilityTracer
	{
	public:
		// A result per group, in order.
		using Callback = std::function<void(std::vector<VisibilityResult>)>;

		virtual ~VisibilityTracer() = default;
		// Traces the groups; a_done runs as a TBB task in the arena the tracer was made in.
		virtual void Trace(std::vector<VisibilityGroup> a_groups, Callback a_done) = 0;
		// Returns once every trace so far has called back.
		virtual void Wait() = 0;
		// Why the GPU failed part way, or empty.
		[[nodiscard]] virtual std::string Problem() const = 0;
	};

	// a_radius: a target farther than this (horizontally, to its nearest point) from every viewpoint of a group isn't
	// traced and counts as visible; 0 traces all. Throws UnsupportedError when the adapter can't trace rays.
	[[nodiscard]] std::unique_ptr<VisibilityTracer> CreateVisibilityTracer(Context& a_context, const VisibilityGround& a_ground,
		const VisibilityTargets& a_targets, float a_radius);
}