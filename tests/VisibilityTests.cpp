// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/VisibilityGpu.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

using namespace tesworld;
using namespace tesworld::gpu;

namespace
{
	void SetFailAfter(const char* a_frames)
	{
#if defined(_WIN32)
		_putenv_s("TESWORLD_GPU_FAIL_AFTER", a_frames);
#else
		if (*a_frames) setenv("TESWORLD_GPU_FAIL_AFTER", a_frames, 1);
		else unsetenv("TESWORLD_GPU_FAIL_AFTER");
#endif
	}

	// Flat ground 640 units square at 0, but for a wall 100 high along x = 320, and 8 x 8 targets 80 across just above it.
	VisibilityGround Ground()
	{
		VisibilityGround ground{ .columns = 65, .rows = 65, .spacing = 10.0f };
		ground.heights.assign(65 * 65, 0.0f);
		for (std::uint32_t y = 0; y < 65; ++y) ground.heights[32 * 65 + y] = 100.0f;
		return ground;
	}

	VisibilityTargets Targets()
	{
		VisibilityTargets targets{ .columns = 8, .rows = 8, .size = 80.0f, .samples = 4 };
		targets.tops.assign(64, 1.0f);
		return targets;
	}

	std::vector<VisibilityResult> TraceAll(VisibilityTracer& a_tracer, std::vector<VisibilityGroup> a_groups)
	{
		std::mutex mutex;
		std::vector<VisibilityResult> results;
		a_tracer.Trace(std::move(a_groups), [&](std::vector<VisibilityResult> a_results) {
			std::scoped_lock lock(mutex);
			results = std::move(a_results);
		});
		a_tracer.Wait();
		return results;
	}

	bool Visible(const VisibilityResult& a_result, std::uint32_t a_x, std::uint32_t a_y) { return a_result.visible.at(a_x * 8 + a_y) != 0; }

	// From west of the wall, low: what's before it is seen and what's behind it isn't; from both sides, everything is;
	// past the radius, nothing is traced and all is shown; a trace of more groups than a frame takes comes back whole.
	void TheWallHidesWhatIsBehindIt()
	{
		try {
			Context context;
			auto tracer = CreateVisibilityTracer(context, Ground(), Targets(), 0.0f);
			const VisibilityGroup west{ { { 50.0f, 320.0f, 10.0f } } };
			const VisibilityGroup both{ { { 50.0f, 320.0f, 10.0f }, { 600.0f, 320.0f, 10.0f } } };
			auto results = TraceAll(*tracer, { west, both });
			assert(results.size() == 2 && results[0].error.empty() && results[1].error.empty());
			for (std::uint32_t y = 0; y < 8; ++y) {
				for (std::uint32_t x = 0; x < 8; ++x) {
					assert(Visible(results[0], x, y) == (x < 4));
					assert(Visible(results[1], x, y));
				}
			}

			auto near = CreateVisibilityTracer(context, Ground(), Targets(), 200.0f);
			results = TraceAll(*near, { west });
			for (std::uint32_t y = 0; y < 8; ++y) {
				for (std::uint32_t x = 0; x < 8; ++x) assert(Visible(results[0], x, y));
			}

			std::vector<VisibilityGroup> many(700, west);
			results = TraceAll(*tracer, many);
			assert(results.size() == many.size());
			for (const auto& result : results) {
				for (std::uint32_t x = 0; x < 8; ++x) assert(Visible(result, x, 0) == (x < 4));
			}
			assert(tracer->Problem().empty());
		} catch (const UnsupportedError& e) {
			std::cout << "no ray tracing, its tests skipped: " << e.what() << '\n';
		}
	}

	// A GPU that fails part way calls back every group with the reason, for the caller to work out itself.
	void AFailingGpuSaysSo()
	{
		// The ground's, indices' and tops' uploads and the structures' build are frames 0 to 3; the first trace fails.
		SetFailAfter("4");
		try {
			Context context;
			auto tracer = CreateVisibilityTracer(context, Ground(), Targets(), 0.0f);
			const auto results = TraceAll(*tracer, std::vector<VisibilityGroup>(3, VisibilityGroup{ { { 50.0f, 320.0f, 10.0f } } }));
			assert(results.size() == 3);
			for (const auto& result : results) assert(result.visible.empty() && !result.error.empty());
			assert(!tracer->Problem().empty());
			std::cout << "after failing: " << tracer->Problem() << '\n';
		} catch (const UnsupportedError& e) {
			std::cout << "no ray tracing, its tests skipped: " << e.what() << '\n';
		}
		SetFailAfter("");
	}
}

int main()
{
	TheWallHidesWhatIsBehindIt();
	AFailingGpuSaysSo();
	std::cout << "tesworld visibility tests passed\n";
	return 0;
}