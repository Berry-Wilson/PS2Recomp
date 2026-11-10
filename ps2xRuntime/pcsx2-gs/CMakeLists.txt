# ps2x-gs.dll: PCSX2's GS as a library for the runtime. This is not part of the runtime's own build:
# it is built inside a PCSX2 source tree, whose root CMakeLists.txt adds it with
#   if(PS2X_GS_DIR) add_subdirectory("${PS2X_GS_DIR}" ps2x-gs) endif()
# and configured with -DPS2X_GS_DIR=<this folder>. build-ps2x-gs.bat here does both steps.
add_library(ps2x-gs SHARED Ps2xGS.cpp)

target_include_directories(ps2x-gs PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${CMAKE_SOURCE_DIR}/pcsx2-gsrunner"
	"${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(ps2x-gs PRIVATE
	PCSX2_FLAGS
	PCSX2
)
