#ifndef _DCM_PHYSICS_MATH_HPP
#define _DCM_PHYSICS_MATH_HPP

#include "pico/stdlib.h"

// units are mm, decimal degrees because fuck you that's why
struct cylindrical_coordinates
{
    int16_t z;
    uint16_t radius;
    uint16_t omega;
};

struct cartesian_coordinates
{
    int16_t x;
    int16_t y;
    int16_t z;
};

extern cartesian_coordinates transform_cylindrical_to_cartesian(cylindrical_coordinates pos_in);
extern cylindrical_coordinates transform_cartesian_to_cylindrical(cartesian_coordinates pos_in);

#endif
