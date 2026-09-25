#pragma once
// The scene: a stack of slots, each holding a stack of layers
// (docs/RENDERER.md sections 4-5). Slots composite bottom (0) to top; within a
// slot, layers composite bottom (0) to top. In M2 a slot is "pass-through":
// its layers blend straight onto what's below, with the slot's opacity
// applied to each. Isolated groups (needed for per-slot crossfades) come with
// modes and transitions in M5.
//
// Everything is fixed-size: no allocation after startup.

#include <cstdint>
#include <span>

#include "neotree/color.hpp"
#include "neotree/entity.hpp"
#include "neotree/geometry.hpp"
#include "neotree/mask.hpp"
#include "neotree/types.hpp"

namespace neotree {

constexpr uint8_t max_slots = 4;
constexpr uint8_t max_layers_per_slot = 6;
// Pixel layers each need a per-LED buffer (4 bytes x 1000 LEDs); this many
// can exist at once across all slots.
constexpr uint8_t max_pixel_buffers = 6;

enum class LayerType : uint8_t
{
    empty,
    solid,   // one color everywhere (within the mask)
    pixel,   // a stored color + coverage per LED
    field,   // a color computed from each LED's position (and time)
    entity,  // the entities assigned to this layer (Entity::slot/layer)
};

enum class FieldKind : uint8_t
{
    height_gradient,   // color_a at the lowest LED -> color_b at the highest
    angle_rainbow,     // hue around the trunk, optionally spinning
};

struct FieldParams
{
    FieldKind kind = FieldKind::height_gradient;
    Rgb color_a{};
    Rgb color_b{};
    // angle_rainbow
    float saturation = 1.0f;
    float value = 1.0f;
    float hue_cycles = 1.0f;   // full hue cycles per trip around the trunk
    float spin_rps = 0.0f;     // turns per second; positive = counter-clockwise seen from above
};

struct Layer
{
    LayerType type = LayerType::empty;
    bool enabled = true;
    Blend blend = Blend::normal;
    float opacity = 1.0f;
    Mask mask{};
    Rgb color{};          // solid
    int8_t buffer = -1;   // pixel: which pixel buffer
    FieldParams field{};  // field
    EntityCombine combine = EntityCombine::add;   // entity: how overlapping entities combine
};

struct Slot
{
    bool enabled = false;
    float opacity = 1.0f;
    uint8_t layer_count = 0;
    Layer layers[max_layers_per_slot];
};

class Scene
{
public:
    Scene() { clear(); }

    // Empties every slot and frees every pixel buffer.
    void clear();
    void clear_slot(uint8_t s);

    Slot *slot(uint8_t s) { return s < max_slots ? &slots_[s] : nullptr; }
    const Slot *slot(uint8_t s) const { return s < max_slots ? &slots_[s] : nullptr; }

    // Null unless slot s has a layer l.
    Layer *layer(uint8_t s, uint8_t l);
    const Layer *layer(uint8_t s, uint8_t l) const;

    // Appends a layer of the given type on top of slot s (enabling the slot)
    // and returns its index, or -1 if the slot is full or (for pixel layers)
    // no pixel buffer is free. New pixel layers start fully transparent.
    int add_layer(uint8_t s, LayerType type);

    // A pixel layer's per-LED contents (max_leds entries), writable; empty
    // for anything that isn't a pixel layer.
    std::span<Rgba8> pixels(uint8_t s, uint8_t l);
    std::span<const Rgba8> pixels(const Layer &layer) const;

    uint8_t pixel_buffers_free() const;

private:
    Slot slots_[max_slots];
    Rgba8 buffers_[max_pixel_buffers][LedGeometry::max_leds];
    bool buffer_used_[max_pixel_buffers] = {};
};

// Optional timing of composite's parts (EngineConfig::profile_clock).
struct CompositeProfile
{
    uint32_t (*clock)() = nullptr;
    uint64_t entity_draw = 0;   // drawing entities into their scratch buffers
};

// Composites the scene into out (one entry per LED, starting from black).
// time_us drives animated fields; entity layers draw from entities, using
// scratch. Returns the number of LED-layer and LED-entity evaluations (the
// platform-independent cost measure).
uint32_t composite(const Scene &scene, const LedGeometry &geometry, int64_t time_us, const EntityPool &entities,
                   EntityScratch &scratch, std::span<Rgb> out, CompositeProfile *profile = nullptr);

}  // namespace neotree
