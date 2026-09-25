#pragma once
// NEOTREE_HOT marks the per-LED inner loops. A platform can place them in
// fast memory by defining NEOTREE_HOT_IN_RAM=1, so they run from SRAM instead
// of flash. The Pico build leaves it off: it gained little, and moving code
// changes flash-cache conflicts elsewhere (docs/RENDERER.md 15, "Snow
// performance"). Elsewhere it does nothing.

// Section attributes are ignored on template instantiations, so hot templates
// are NEOTREE_INLINE and get inlined into a NEOTREE_HOT (non-template) caller.
#define NEOTREE_INLINE inline __attribute__((always_inline))

#if defined(NEOTREE_HOT_IN_RAM) && NEOTREE_HOT_IN_RAM
// The Pico SDK's linker scripts copy .time_critical.* sections into RAM.
#define NEOTREE_HOT __attribute__((section(".time_critical.neotree"), noinline))
#else
#define NEOTREE_HOT
#endif
