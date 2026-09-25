# Engine unit tests (host only - built from sim/CMakeLists.txt). Uses the
# single-header doctest, downloaded once into the build tree.

set(DOCTEST_DIR ${CMAKE_BINARY_DIR}/_deps/doctest)
set(DOCTEST_HEADER ${DOCTEST_DIR}/doctest/doctest.h)
if (NOT EXISTS ${DOCTEST_HEADER})
    file(DOWNLOAD https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h ${DOCTEST_HEADER}
         STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if (NOT code EQUAL 0)
        file(REMOVE ${DOCTEST_HEADER})
        message(FATAL_ERROR "downloading doctest failed: ${status}")
    endif()
endif()

add_executable(neotree_engine_tests
    test_main.cpp
    test_clock.cpp
    test_random.cpp
    test_pool.cpp
    test_geometry.cpp
    test_engine.cpp
    test_color.cpp
    test_mask.cpp
    test_scene.cpp
    test_entity.cpp
)
target_include_directories(neotree_engine_tests PRIVATE ${DOCTEST_DIR})
target_link_libraries(neotree_engine_tests PRIVATE neotree_engine)
# doctest itself needs exceptions; the engine library keeps its own
# -fno-exceptions, this just re-enables them for the test executable.
target_compile_options(neotree_engine_tests PRIVATE -fexceptions -frtti -Wall -Wextra)
add_test(NAME engine COMMAND neotree_engine_tests)
