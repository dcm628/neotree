#ifndef _NEO_TREE_CONFIG_HPP
#define _NEO_TREE_CONFIG_HPP

#include "pico/stdlib.h"
#include <array>
using std::array;

#include "dcm_physics_math.hpp"

struct string_led_config
{
    uint16_t string_position;
    cylindrical_coordinates coordinates;
}__packed;

enum class config_type : uint32_t
{
    string_position = 1,
};

struct revision_info
{
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
}__packed;

struct neo_tree_config_header
{
    revision_info rev;
    config_type type_id;
    uint32_t size_bytes;
}__packed;

const size_t max_led_config_size = 500;
struct neo_tree_pos_config_data
{
    neo_tree_config_header config_header;
    uint32_t max_pos_index;
    std::array<string_led_config, max_led_config_size> tree_config_array;
}__packed;

string_led_config lookup_pos_config(uint16_t string_position_in);
bool write_flash_pos_config(string_led_config set_config);

// Place it in a custom section so the linker can control where it's placed
__attribute__((section(".config_data_section"))) extern neo_tree_pos_config_data posConfigData;

// Define the constexpr function for default initialization
constexpr neo_tree_pos_config_data get_default_tree_pos_config_data() {
    neo_tree_pos_config_data config_data = {};

    // Initialize config_header fields
    config_data.config_header.rev = {1, 0, 0}; // revision info (major, minor, patch)
    config_data.config_header.type_id = config_type::string_position;
    config_data.config_header.size_bytes = sizeof(neo_tree_pos_config_data); // Set the size in bytes

    config_data.max_pos_index = max_led_config_size;

    // Initialize tree_config_array with default values
    for (size_t i = 0; i < max_led_config_size; ++i) {
        config_data.tree_config_array[i].string_position = static_cast<uint16_t>(i); // Example string position
        config_data.tree_config_array[i].coordinates = {static_cast<int16_t>(i), static_cast<uint16_t>(i), static_cast<uint16_t>(i)}; // Default cylindrical coordinates (x=0, y=0, z=0), incrementing with i to allow a test sweep in any axis to work as a string sweep
    }

    return config_data;
}


#endif