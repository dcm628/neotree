# NeoTree rendering engine - portable C++20, no Pico SDK dependencies.
# Built into the firmware (firmware/CMakeLists.txt) and the host simulator
# (sim/CMakeLists.txt). See docs/RENDERER.md.
#
# Rules for code in here, so it builds identically for both targets: no
# exceptions, no RTTI, no heap allocation after init, no iostream, float
# (not double) in anything that runs per tick or per frame.

add_library(neotree_engine STATIC
    src/engine.cpp
    src/geometry.cpp
    src/mask.cpp
    src/scene.cpp
)

target_include_directories(neotree_engine PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(neotree_engine PUBLIC cxx_std_20)
target_compile_options(neotree_engine PRIVATE -Wall -Wextra -Wdouble-promotion)

# The Pico SDK already builds without exceptions/RTTI; enforce the same on the
# host so anything that would fail in firmware fails here first. PRIVATE: the
# host tools and tests that link the engine can still use them.
if (NOT CMAKE_CROSSCOMPILING)
    target_compile_options(neotree_engine PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()
