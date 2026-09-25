# NeoTree rendering engine - portable C++20, no Pico SDK dependencies.
# Built into the firmware (firmware/CMakeLists.txt) and the host simulator
# (sim/CMakeLists.txt). See docs/RENDERER.md.
#
# Rules for code in here, so it builds identically for both targets: no
# exceptions, no RTTI, no heap allocation after init, no iostream, float
# (not double) in anything that runs per tick or per frame.

add_library(neotree_engine STATIC
    src/behavior.cpp
    src/direct.cpp
    src/effect.cpp
    src/director.cpp
    src/library.cpp
    src/engine.cpp
    src/entity.cpp
    src/geometry.cpp
    src/modes.cpp
    src/mask.cpp
    src/scene.cpp
)

target_include_directories(neotree_engine PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(neotree_engine PUBLIC cxx_std_20)
# -fno-math-errno: the engine never reads errno, and without it sqrtf etc.
# compile to single FPU instructions instead of calls that may set errno.
target_compile_options(neotree_engine PRIVATE -Wall -Wextra -Wdouble-promotion -fno-math-errno)

# The Pico SDK already builds without exceptions/RTTI; enforce the same on the
# host so anything that would fail in firmware fails here first. PRIVATE: the
# host tools and tests that link the engine can still use them.
if (NOT CMAKE_CROSSCOMPILING)
    target_compile_options(neotree_engine PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()
