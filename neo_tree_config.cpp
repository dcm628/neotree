#include "neo_tree_config.hpp"
#include <stdio.h>
#include "hardware/flash.h"
#include "hardware/sync.h"

extern uint32_t __config_data_start;  // Declare the start symbol
extern uint32_t __config_data_end;    // Declare the end symbol

//std::array<string_led_config, max_led_config_size> tree_config_array;
neo_tree_pos_config_data posConfigData = get_default_tree_pos_config_data();

string_led_config lookup_pos_config(uint16_t string_position_in)
{
    bool match_found = false;
    string_led_config return_config;
    uint32_t interrupts = save_and_disable_interrupts();
    for (auto & element : posConfigData.tree_config_array) {
        if (element.string_position == string_position_in)
        {
            match_found = true;
            return_config = element;
        }
    }
    if (match_found == false)
    {
    return_config.string_position = 0;
    return_config.coordinates.omega = 0;
    return_config.coordinates.radius = 0;
    return_config.coordinates.z = 0;
    }
    restore_interrupts(interrupts);
    return return_config;
}

bool verify_pos_config(string_led_config set_config, string_led_config read_config)
{
    uint8_t check_val = (set_config.string_position != set_config.string_position)
     + (set_config.coordinates.omega != set_config.coordinates.omega)
     + (set_config.coordinates.radius != set_config.coordinates.radius)
     + (set_config.coordinates.z != set_config.coordinates.z);
    if (check_val != 0)
    {
        // something didn't match
        return false;
    }
    else return true;
}

bool write_flash_pos_config(string_led_config set_config)
{
    bool write_success = false; // default false
    // verify index in range
    printf("input config index: ");
    printf("%d\n", set_config.string_position);
    uint32_t interrupts = save_and_disable_interrupts();
    printf("interrupts saved and disabled ");
    //if (set_config.string_position <= posConfigData.max_pos_index)
    if (set_config.string_position <= 499)
    {
        // in bounds - copy config to ram to modify
        neo_tree_pos_config_data posConfigDataCopy;// = posConfigData;
        // in bounds - attempt to write
        printf("in bounds - proceed with write attempt ");
        posConfigDataCopy.tree_config_array.at(set_config.string_position) = set_config;
        // clear flash page - necessary before writing - assuming single flash page for erase and write
        printf("config copied to stack member  ");
        //uint32_t start_address = (uint32_t)&__config_data_start;
        uint32_t start_address = (uint32_t)&posConfigData - 0x10000000;
    // Calculate the size of the config data section
    uint32_t end_address = (uint32_t)&__config_data_end;
    uint32_t config_page_size = end_address - start_address;        
        printf("start_address stack member set ");
        printf("start_address: ");
        printf("%d\n", start_address);
        flash_range_erase(start_address, FLASH_SECTOR_SIZE);
        //flash_range_erase(start_address, config_page_size);
        printf("flash erase complete ");
        flash_range_program(start_address, (const uint8_t*)&posConfigDataCopy, FLASH_PAGE_SIZE);
        printf("flash write complete");
        restore_interrupts(interrupts);
        printf("interrupts restored");
        // read back data to verify
        write_success = verify_pos_config(set_config, posConfigData.tree_config_array.at(set_config.string_position));
        printf("verify write_success complete ");
    }
    return write_success;
}