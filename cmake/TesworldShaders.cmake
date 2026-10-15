# tesworld's GPU shaders, compiled at build time and embedded in the library: DXIL for D3D12 (Windows) and SPIR-V for
# Vulkan, each looked up at runtime by "<name>.dxil" / "<name>.spv" (src/gpu/Shaders.h).
#
#   tesworld_gpu_shader(<list var> NAME <name> SOURCE <file.hlsl> ENTRY <entry> [PROFILE <cs_6_6|vs_6_6|ps_6_6>] [DEPENDS <headers>...])
#
# A graphics program's stages are named "<program>.VS" and "<program>.PS" (ContextImpl::CreateGraphicsProgram).
#   tesworld_embed_shaders(<target> <list var>)
include_guard(GLOBAL)

function(tesworld_gpu_shader a_outputs)
	cmake_parse_arguments(ARG "" "NAME;SOURCE;ENTRY;PROFILE" "DEPENDS" ${ARGN})
	if(NOT ARG_PROFILE)
		set(ARG_PROFILE cs_6_6)
	endif()
	set(_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	set(_outputs ${${a_outputs}})
	get_filename_component(_include "${ARG_SOURCE}" DIRECTORY)
	if(WIN32)
		set(_dxil "${_dir}/${ARG_NAME}.dxil")
		add_custom_command(
			OUTPUT "${_dxil}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
			COMMAND "${TESWORLD_DXC_EXECUTABLE}" -nologo -HV 2021 -T ${ARG_PROFILE} -E ${ARG_ENTRY} -WX -O3 -I "${_include}"
				-D BASICRHI_SHADER_API_D3D12=1 -Fo "${_dxil}" "${ARG_SOURCE}"
			DEPENDS "${ARG_SOURCE}" ${ARG_DEPENDS}
			COMMENT "DXIL ${ARG_ENTRY} <- ${ARG_SOURCE}"
			VERBATIM)
		list(APPEND _outputs "${_dxil}")
	endif()
	set(_spv "${_dir}/${ARG_NAME}.spv")
	basicrhi_compile_spirv(OUTPUT "${_spv}" SOURCE "${ARG_SOURCE}" ENTRY ${ARG_ENTRY} PROFILE ${ARG_PROFILE} DXC "${TESWORLD_DXC_EXECUTABLE}"
		INCLUDE_DIRS "${_include}" DEPENDS ${ARG_DEPENDS})
	list(APPEND _outputs "${_spv}")
	set(${a_outputs} ${_outputs} PARENT_SCOPE)
endfunction()

function(tesworld_embed_shaders a_target a_files)
	set(_generated "${CMAKE_CURRENT_BINARY_DIR}/shaders/EmbeddedShaders.cpp")
	string(JOIN "|" _list ${${a_files}})
	add_custom_command(
		OUTPUT "${_generated}"
		COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${_generated}" "-DFILES=${_list}" -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
		DEPENDS ${${a_files}} "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
		COMMENT "Embedding tesworld's shaders"
		VERBATIM)
	target_sources(${a_target} PRIVATE "${_generated}")
endfunction()
