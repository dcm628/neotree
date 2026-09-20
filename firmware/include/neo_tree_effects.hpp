#ifndef _NEO_TREE_EFFECTS_HPP
#define _NEO_TREE_EFFECTS_HPP

#include "pico/stdlib.h"
#include "dcm_physics_math.hpp"

enum class sweep_type
{
    CYLINDRICAL,
    CARTESIAN,
};

class neo_sweep
{
private:
    cylindrical_coordinates min_region_coordinates;
    cylindrical_coordinates man_region_coordinates;
    uint32_t render_steps;
    uint32_t render_step_counter;
public:
    neo_sweep(  cylindrical_coordinates set_min_coordinates,
                cylindrical_coordinates set_max_coordinates,
                cylindrical_coordinates set_sweep_start_coordinates,
                cylindrical_coordinates set_sweep_end_coordinates,
                uint16_t sweep_size_percent,
                uint32_t duration_ms,
                uint32_t target_loop_rate);
    bool sequence_update();
    ~neo_sweep();
};


#endif