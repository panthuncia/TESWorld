# BasicTelemetry, for profiling (TESWORLD_WITH_PROFILING) and for OpenRenderGraph (TESWORLD_WITH_GPU), which links its
# Tracy passthrough. A host that already provides these targets (FasterNGIO, SARP) keeps its own.
include_guard(GLOBAL)

if(TESWORLD_WITH_PROFILING)
	# The real client: zones reach Tracy (not on demand, so a capture sees a run from its start).
	find_package(Tracy CONFIG REQUIRED)
elseif(NOT TARGET Tracy::TracyClient)
	# Tracy's headers alone, without TRACY_ENABLE: every zone macro compiles to nothing, and the executable carries no
	# client listening on a port. find_package(Tracy) later finds the target defined and adds nothing.
	find_path(TESWORLD_TRACY_INCLUDE_DIR tracy/Tracy.hpp PATH_SUFFIXES tracy REQUIRED)
	add_library(Tracy::TracyClient INTERFACE IMPORTED GLOBAL)
	set_target_properties(Tracy::TracyClient PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${TESWORLD_TRACY_INCLUDE_DIR}")
endif()

if(NOT TARGET BasicTelemetry::Core)
	FetchContent_Declare(BasicTelemetry
		GIT_REPOSITORY https://github.com/panthuncia/BasicTelemetry.git
		GIT_TAG 3bd82d9884cbc693a2cf98a566882e705f801425
		EXCLUDE_FROM_ALL
		SYSTEM)
	set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(BASICTELEMETRY_ENABLE_TRACY ON CACHE BOOL "" FORCE)
	# The JSON/SQLite summary (--telemetry) is for profiling builds only.
	set(BASICTELEMETRY_BUILD_ARTIFACTS ${TESWORLD_WITH_PROFILING} CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(BasicTelemetry)
endif()
