# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.0)
set(toolchainVersion 13_3_Rel1)
set(picotoolVersion 2.1.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(neo_tree C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(neo_tree)


file(MAKE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/generated)

# generate the header file into the source tree as it is included in the RP2040 datasheet
pico_generate_pio_header(neo_tree ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/generated)

target_sources(neo_tree PRIVATE
                src/main.cpp
                src/dcm_rgb.cpp
                src/neo_tree_config.cpp
                src/dcm_physics_math.cpp
                src/neo_tree_serial.cpp
                src/neo_tree_effects.cpp
                )

# Project headers live in include/
target_include_directories(neo_tree PRIVATE
                ${CMAKE_CURRENT_LIST_DIR}/include
                )


target_compile_definitions(neo_tree PRIVATE
        #PIN_DBG1=3
        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
        )

target_compile_options(neo_tree PRIVATE
#-Os
#-flto  # Enable Link-Time Optimization
#-funroll-loops  # Enable loop unrolling
)

target_link_libraries(neo_tree PRIVATE    pico_stdlib
                                            hardware_pio
                                            # hardware_dma
                                            pico_multicore
                                            # pico_cyw43_arch_lwip_threadsafe_background
                                            pico_cyw43_arch_none
                                            hardware_flash)

# enable usb output, disable uart output
pico_enable_stdio_usb(neo_tree 1)
pico_enable_stdio_uart(neo_tree 0)

pico_add_extra_outputs(neo_tree)

pico_set_linker_script(neo_tree ${CMAKE_CURRENT_LIST_DIR}/memmap_custom.ld)

# add url via pico_set_program_url
# example_auto_set_url(neo_tree)

# Additionally generate python and hex pioasm outputs for inclusion in the RP2040 datasheet
add_custom_target(pio_ws2812_datasheet DEPENDS ${CMAKE_CURRENT_LIST_DIR}/generated/ws2812.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_LIST_DIR}/generated/ws2812.py
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio
        COMMAND pioasm -o python ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio ${CMAKE_CURRENT_LIST_DIR}/generated/ws2812.py
        VERBATIM)
add_dependencies(neo_tree pio_ws2812_datasheet)