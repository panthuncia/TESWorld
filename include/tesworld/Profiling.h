#pragma once

// Profiling zones. Built with TESWORLD_WITH_PROFILING (CMake), a zone is a BasicTelemetry scope passed through to Tracy;
// without it every macro here compiles to nothing, so a shipped build carries no profiler.
//
//   TW_ZONE("terrain block");                          // a zone to the end of the enclosing scope
//   TW_ZONE_TEXT("terrain block", "{} {},{}", l, x, y); // the same, with text (formatted only when profiling)
//   TW_ZONE_DYNAMIC(name);                             // a zone named at run time (a std::string), Tracy only
//   TW_PLOT("blocks in flight", n);
//   TW_NAME_WORKER_THREADS();                          // TBB's workers named in Tracy while this scope lasts
//   TW_PHASES(phase);                                  // a function's steps, one zone at a time:
//   TW_PHASE(phase, "atlas: pack");                    //   ends the step before and begins this one; the last
//                                                      //   ends with the scope (Tracy only, not BasicTelemetry)
//
// Each zone has its own variable, so zones nest in one function without shadowing.

#if defined(TESWORLD_PROFILING)

#include <BasicTelemetry/Tracy.h>

#include <oneapi/tbb/task_scheduler_observer.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace tesworld::profiling {

inline void Text(tracy::ScopedZone& a_zone, std::string_view a_text)
{
	::basic_telemetry::AnnotateCurrentScope(a_text);
	a_zone.Text(a_text.data(), a_text.size());
}

// Names TBB's worker threads as they join the arena ("TBB worker N"), so Tracy tells them from other threads (such as
// OpenMP's). Keep one alive for the run.
class NameWorkerThreads final : public tbb::task_scheduler_observer {
public:
	NameWorkerThreads() { observe(true); }
	~NameWorkerThreads() override { observe(false); }

	void on_scheduler_entry(bool a_worker) override
	{
		if (!a_worker) {
			return;
		}
		thread_local bool named = false;
		if (!named) {
			named = true;
			const auto name = std::format("TBB worker {}", _next.fetch_add(1));
			tracy::SetThreadName(name.c_str()); // Tracy keeps a copy
		}
	}

private:
	std::atomic<int> _next{ 1 };
};

}

#define TW_JOIN_IMPL(a, b) a##b
#define TW_JOIN(a, b) TW_JOIN_IMPL(a, b)
#define TW_ZONE(name) BT_ZONE_NAMED(TW_JOIN(_twZone, __LINE__), name)
#define TW_ZONE_TEXT(name, ...)                         \
	BT_ZONE_NAMED(TW_JOIN(_twZone, __LINE__), name); \
	::tesworld::profiling::Text(TW_JOIN(_twZone, __LINE__), std::format(__VA_ARGS__))
#define TW_ZONE_DYNAMIC(name)                                                                                                 	const std::string_view TW_JOIN(_twName, __LINE__){ name };                                                                 	tracy::ScopedZone TW_JOIN(_twZone, __LINE__)(static_cast<std::uint32_t>(__LINE__), TracyFile, sizeof(TracyFile) - 1,       		TracyFunction, std::char_traits<char>::length(TracyFunction), TW_JOIN(_twName, __LINE__).data(),                    		TW_JOIN(_twName, __LINE__).size(), -1, true)
#define TW_PLOT(name, value) BT_PLOT(name, value)
#define TW_THREAD_NAME(name) tracy::SetThreadName(name)
#define TW_NAME_WORKER_THREADS() const ::tesworld::profiling::NameWorkerThreads TW_JOIN(_twWorkers, __LINE__)
#define TW_PHASES(var) std::optional<tracy::ScopedZone> var
#define TW_PHASE(var, name)                                                                                                    	do {                                                                                                                       		var.reset();                                                                                                           		static constexpr tracy::SourceLocationData TW_JOIN(_twPhase, __LINE__){ name, TracyFunction, TracyFile,                 			static_cast<std::uint32_t>(__LINE__), 0 };                                                                         		var.emplace(&TW_JOIN(_twPhase, __LINE__));                                                                             	} while (false)

#else

#define TW_ZONE(name) static_cast<void>(0)
#define TW_ZONE_TEXT(name, ...) static_cast<void>(0)
#define TW_ZONE_DYNAMIC(name) static_cast<void>(0)
#define TW_PLOT(name, value) static_cast<void>(0)
#define TW_THREAD_NAME(name) static_cast<void>(0)
#define TW_NAME_WORKER_THREADS() static_cast<void>(0)
#define TW_PHASES(var) static_cast<void>(0)
#define TW_PHASE(var, name) static_cast<void>(0)

#endif
