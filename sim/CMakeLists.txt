# Host (PC) build of the rendering engine: the headless simulator, the 3D
# viewer, and the engine's unit tests. Build with tools/build_sim.ps1, or:
#   cmake -S sim -B sim/build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
#   cmake --build sim/build && ctest --test-dir sim/build
# See sim/README.md.

cmake_minimum_required(VERSION 3.25)
# raylib's bundled GLFW still declares an old minimum CMake version, which
# CMake 4 otherwise refuses.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(neotree_sim C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (NOT CMAKE_BUILD_TYPE)
    # Optimized by default: the point of the simulator is running fast.
    set(CMAKE_BUILD_TYPE Release)
endif()

option(NEOTREE_BUILD_VIEWER "Build the raylib 3D viewer" ON)
option(NEOTREE_BUILD_TESTS "Build the engine unit tests" ON)

# MinGW: link the runtime statically so the tools run without the compiler's
# DLLs on PATH.
if (MINGW)
    add_link_options(-static)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../engine ${CMAKE_BINARY_DIR}/engine)

# Shared by the CLI and viewer: loading positions, argument helpers.
add_library(neotree_sim_common STATIC common/positions_csv.cpp common/demo_scenes.cpp)
target_include_directories(neotree_sim_common PUBLIC ${CMAKE_CURRENT_LIST_DIR}/common)
target_link_libraries(neotree_sim_common PUBLIC neotree_engine)
target_compile_definitions(neotree_sim_common PUBLIC
    NEOTREE_SIM_DEFAULT_POSITIONS="${CMAKE_CURRENT_LIST_DIR}/data/tree_positions.csv")

add_executable(neotree_sim cli/main.cpp)
target_link_libraries(neotree_sim PRIVATE neotree_sim_common)
target_compile_options(neotree_sim PRIVATE -Wall -Wextra)

if (NEOTREE_BUILD_VIEWER)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
        URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(raylib)
    # Third-party: its bundled audio decoders warn under GCC 16. Silenced so
    # our own warnings stay visible.
    target_compile_options(raylib PRIVATE -w)

    add_executable(neotree_view viewer/main.cpp)
    target_link_libraries(neotree_view PRIVATE neotree_sim_common raylib)
    target_compile_options(neotree_view PRIVATE -Wall -Wextra)
endif()

if (NEOTREE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../engine/tests ${CMAKE_BINARY_DIR}/engine_tests)
endif()
