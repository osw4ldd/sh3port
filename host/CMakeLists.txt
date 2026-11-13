# ps2native host: the ps2native runtime + a recompiled program, linked with the
# PCSX2 core.  Configured from PCSX2's top-level CMakeLists via
#   -DPS2NATIVE_HOST_DIR=<ps2native>/host
#   -DPS2NATIVE_GAME_DIR=<recompiler output dir>   (contains gen/)
#   -DPS2NATIVE_GAME_NAME=<exe name without extension>

set(PS2NATIVE_RUNTIME "${CMAKE_CURRENT_LIST_DIR}/../runtime")

# Runtime core (C).  Compiled without PCSX2's flags; plain C11.
add_library(ps2native-rt STATIC
	"${PS2NATIVE_RUNTIME}/src/core/dispatch.c"
	"${PS2NATIVE_RUNTIME}/src/core/kernel.c"
	"${PS2NATIVE_RUNTIME}/src/core/memory.c"
	"${PS2NATIVE_RUNTIME}/src/core/rtmain.c"
	"${PS2NATIVE_RUNTIME}/src/core/sif.c"
	"${PS2NATIVE_RUNTIME}/src/core/sif_lle.c"
)
target_include_directories(ps2native-rt PUBLIC "${PS2NATIVE_RUNTIME}/include" "${PS2NATIVE_RUNTIME}/src")
target_compile_definitions(ps2native-rt PUBLIC PS2_IMAGE_RESOURCE _CRT_SECURE_NO_WARNINGS)
set_target_properties(ps2native-rt PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(ps2native-rt PRIVATE /O2 /W3 /wd4146 /wd4244 /wd4018 /wd4996)
target_link_libraries(ps2native-rt PUBLIC winmm)

if(PS2NATIVE_GAME_DIR)
	file(TO_CMAKE_PATH "${PS2NATIVE_GAME_DIR}" PS2NATIVE_GAME_DIR)
	if(NOT PS2NATIVE_GAME_NAME)
		set(PS2NATIVE_GAME_NAME ps2native-game)
	endif()
	# The recompiler regenerates gen/ and re-runs CMake for every program.
	file(GLOB PS2NATIVE_GAME_SOURCES "${PS2NATIVE_GAME_DIR}/gen/*.c")

	# Recompiled guest code (C).
	add_library(ps2native-game-code STATIC ${PS2NATIVE_GAME_SOURCES})
	target_include_directories(ps2native-game-code PRIVATE "${PS2NATIVE_GAME_DIR}/gen")
	target_link_libraries(ps2native-game-code PUBLIC ps2native-rt)
	set_target_properties(ps2native-game-code PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
	target_compile_options(ps2native-game-code PRIVATE /O2 /W1 /wd4146 /wd4244 /bigobj)

	add_executable(ps2native-game
		Main.cpp
		NativeCpu.cpp
		"${PS2NATIVE_GAME_DIR}/gen/image.rc"
	)
	target_include_directories(ps2native-game PRIVATE
		"${CMAKE_BINARY_DIR}/common/include"
		"${CMAKE_SOURCE_DIR}"
		"${CMAKE_SOURCE_DIR}/pcsx2"
	)
	target_link_libraries(ps2native-game PRIVATE PCSX2_FLAGS PCSX2 ps2native-game-code)
	target_link_options(ps2native-game PRIVATE /STACK:67108864)
	set_target_properties(ps2native-game PROPERTIES
		OUTPUT_NAME "${PS2NATIVE_GAME_NAME}"
		RUNTIME_OUTPUT_DIRECTORY "${PS2NATIVE_GAME_DIR}/bin")
endif()
