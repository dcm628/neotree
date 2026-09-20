#include "dcm_physics_math.hpp"
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

cartesian_coordinates transform_cylindrical_to_cartesian(cylindrical_coordinates pos_in)
{
    struct cartesian_coordinates return_coordinates;

    // Cast radius and omega to double for precision
    double radius = static_cast<double>(pos_in.radius);
    double omega_deg = static_cast<double>(pos_in.omega);

    // Convert omega (in degrees) to radians
    double omega_rad = omega_deg * M_PI / 180.0;

    // Perform the conversion from cylindrical to cartesian
    return_coordinates.x = static_cast<int16_t>(radius * cos(omega_rad));
    return_coordinates.y = static_cast<int16_t>(radius * sin(omega_rad));

    // z remains the same (no casting needed)
    return_coordinates.z = pos_in.z;

    return return_coordinates;
}

// Function to transform cartesian to cylindrical coordinates
cylindrical_coordinates transform_cartesian_to_cylindrical(cartesian_coordinates pos_in)
{
    cylindrical_coordinates return_coordinates;

    // Cast x and y to double for precision
    double x = static_cast<double>(pos_in.x);
    double y = static_cast<double>(pos_in.y);

    // Calculate radius from x and y
    return_coordinates.radius = static_cast<uint16_t>(sqrt(x * x + y * y));

    // Calculate omega (in degrees)
    double omega_rad = atan2(y, x);  // atan2 returns radians
    return_coordinates.omega = static_cast<uint16_t>(omega_rad * 180.0 / M_PI);  // Convert radians to degrees

    // z remains the same (no casting needed)
    return_coordinates.z = pos_in.z;

    return return_coordinates;
}