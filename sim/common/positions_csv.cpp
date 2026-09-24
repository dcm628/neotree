#include "positions_csv.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace neotree::sim {

bool load_positions_csv(const std::string &path, LedGeometry &geometry, std::string &error)
{
    std::ifstream in(path);
    if (!in)
    {
        error = "can't open " + path;
        return false;
    }

    struct Row
    {
        long index;
        LedPoint point;
    };
    std::vector<Row> rows;
    long max_index = -1;
    std::string line;
    int line_no = 0;
    bool header_seen = false;
    while (std::getline(in, line))
    {
        line_no++;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        if (!header_seen)
        {
            header_seen = true;   // index,z_mm,radius_mm,angle_deg,source
            continue;
        }
        std::stringstream ss(line);
        std::string f[5];
        for (auto &field : f)
        {
            std::getline(ss, field, ',');
        }
        char *end = nullptr;
        long index = std::strtol(f[0].c_str(), &end, 10);
        if (end == f[0].c_str() || index < 0 || index >= LedGeometry::max_leds)
        {
            error = path + ":" + std::to_string(line_no) + ": bad LED index '" + f[0] + "'";
            return false;
        }
        PositionSource source;
        if (f[4] == "mapped")
        {
            source = PositionSource::mapped;
        }
        else if (f[4] == "synthetic")
        {
            source = PositionSource::synthetic;
        }
        else if (f[4] == "none")
        {
            source = PositionSource::none;
        }
        else
        {
            error = path + ":" + std::to_string(line_no) + ": unknown source '" + f[4] + "'";
            return false;
        }
        LedPoint p = led_point_from_cylindrical(std::strtof(f[1].c_str(), nullptr), std::strtof(f[2].c_str(), nullptr),
                                                std::strtof(f[3].c_str(), nullptr), source);
        rows.push_back({index, p});
        if (index > max_index)
        {
            max_index = index;
        }
    }
    if (rows.empty())
    {
        error = path + ": no LEDs";
        return false;
    }

    geometry.reset(static_cast<uint16_t>(max_index + 1));
    for (const Row &r : rows)
    {
        geometry.set(static_cast<uint16_t>(r.index), r.point);
    }
    geometry.finalize();
    return true;
}

double parse_duration_s(const std::string &text)
{
    if (text.empty())
    {
        return -1.0;
    }
    char *end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || value < 0.0)
    {
        return -1.0;
    }
    std::string unit(end);
    if (unit.empty() || unit == "s")
    {
        return value;
    }
    if (unit == "m")
    {
        return value * 60.0;
    }
    if (unit == "h")
    {
        return value * 3600.0;
    }
    if (unit == "d")
    {
        return value * 86400.0;
    }
    return -1.0;
}

}  // namespace neotree::sim
