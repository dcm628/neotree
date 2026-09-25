#pragma once
// Minimal JSON writer over a fixed buffer, shared by the engine's describe
// functions (internal - not part of the public headers). Truncates safely:
// a too-small buffer gives cut-off JSON, never an overrun.

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "neotree/color.hpp"

namespace neotree::detail {

struct Json
{
    char *out;
    size_t cap;
    size_t len = 0;

    void raw(const char *fmt, ...)
    {
        if (len + 1 >= cap)
        {
            return;
        }
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(out + len, cap - len, fmt, args);
        va_end(args);
        if (n > 0)
        {
            len = len + static_cast<size_t>(n) < cap ? len + static_cast<size_t>(n) : cap - 1;
        }
    }

    // Numbers the same on every printf: integers plainly, otherwise up to
    // three decimals with trailing zeros trimmed (the Pico's %g pads).
    void num(double v)
    {
        char buf[24];
        long whole = static_cast<long>(v);
        if (static_cast<double>(whole) == v)
        {
            std::snprintf(buf, sizeof(buf), "%ld", whole);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            char *end = buf + std::strlen(buf) - 1;
            while (end > buf && *end == '0')
            {
                *end-- = '\0';
            }
        }
        raw("%s", buf);
    }

    void hex(Rgb c) { raw("\"#%02x%02x%02x\"", unit_to_byte(c.r), unit_to_byte(c.g), unit_to_byte(c.b)); }

    // A quoted string, escaped - for names people typed. Control characters
    // are dropped (the library never stores them anyway).
    void str(const char *s)
    {
        raw("\"");
        for (; *s != '\0'; s++)
        {
            const unsigned char ch = static_cast<unsigned char>(*s);
            if (ch == '"' || ch == '\\')
            {
                raw("\\%c", ch);
            }
            else if (ch >= 0x20)
            {
                raw("%c", ch);
            }
        }
        raw("\"");
    }
};

}  // namespace neotree::detail
