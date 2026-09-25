#include "neotree/scene.hpp"

#include <cmath>

#include "neotree/hot.hpp"

namespace neotree {

void Scene::clear()
{
    for (uint8_t s = 0; s < max_slots; s++)
    {
        slots_[s] = Slot{};
    }
    for (bool &used : buffer_used_)
    {
        used = false;
    }
}

void Scene::clear_slot(uint8_t s)
{
    Slot *slot = this->slot(s);
    if (slot == nullptr)
    {
        return;
    }
    for (uint8_t l = 0; l < slot->layer_count; l++)
    {
        if (slot->layers[l].buffer >= 0)
        {
            buffer_used_[slot->layers[l].buffer] = false;
        }
    }
    *slot = Slot{};
}

Layer *Scene::layer(uint8_t s, uint8_t l)
{
    Slot *slot = this->slot(s);
    return slot != nullptr && l < slot->layer_count ? &slot->layers[l] : nullptr;
}

const Layer *Scene::layer(uint8_t s, uint8_t l) const
{
    const Slot *slot = this->slot(s);
    return slot != nullptr && l < slot->layer_count ? &slot->layers[l] : nullptr;
}

int Scene::add_layer(uint8_t s, LayerType type)
{
    Slot *slot = this->slot(s);
    if (slot == nullptr || slot->layer_count >= max_layers_per_slot || type == LayerType::empty)
    {
        return -1;
    }
    int8_t buffer = -1;
    if (type == LayerType::pixel)
    {
        for (uint8_t b = 0; b < max_pixel_buffers; b++)
        {
            if (!buffer_used_[b])
            {
                buffer = static_cast<int8_t>(b);
                break;
            }
        }
        if (buffer < 0)
        {
            return -1;
        }
        buffer_used_[buffer] = true;
        for (Rgba8 &px : buffers_[buffer])
        {
            px = Rgba8{};
        }
    }
    uint8_t index = slot->layer_count++;
    slot->layers[index] = Layer{};
    slot->layers[index].type = type;
    slot->layers[index].buffer = buffer;
    slot->enabled = true;
    return index;
}

std::span<Rgba8> Scene::pixels(uint8_t s, uint8_t l)
{
    Layer *layer = this->layer(s, l);
    if (layer == nullptr || layer->type != LayerType::pixel || layer->buffer < 0)
    {
        return {};
    }
    return buffers_[layer->buffer];
}

std::span<const Rgba8> Scene::pixels(const Layer &layer) const
{
    if (layer.type != LayerType::pixel || layer.buffer < 0)
    {
        return {};
    }
    return buffers_[layer.buffer];
}

uint8_t Scene::pixel_buffers_free() const
{
    uint8_t n = 0;
    for (bool used : buffer_used_)
    {
        n += used ? 0 : 1;
    }
    return n;
}

namespace {

// Fraction of a full cycle, [0, 1), for rate_per_s cycles per second at
// time_us. Worked in double once per layer per frame, so it stays exact over
// any run length; everything per LED is float.
float cycle_phase(double rate_per_s, int64_t time_us)
{
    double turns = rate_per_s * static_cast<double>(time_us) * 1e-6;
    return static_cast<float>(turns - std::floor(turns));
}

// One layer's loop, specialized per blend mode and per layer type (the
// sample function) so nothing is re-decided per LED. All inlined into
// composite_layer, which is what gets placed in fast memory.
template <Blend Mode, typename Sample>
NEOTREE_INLINE void run_layer(const Layer &layer, float base_alpha, const LedGeometry &geometry, std::span<Rgb> out,
                           uint32_t &evals, Sample sample)
{
    const bool masked = layer.mask.shape != MaskShape::none;
    uint32_t n_evals = 0;
    for (size_t i = 0; i < out.size(); i++)
    {
        float a = base_alpha;
        Rgb src;
        if (!sample(i, src, a))
        {
            continue;
        }
        if (masked)
        {
            a *= mask_coverage(layer.mask, geometry, static_cast<uint16_t>(i));
            if (a <= 0.0f)
            {
                continue;
            }
        }
        n_evals++;
        blend_into(out[i], src, a, Mode);
    }
    evals += n_evals;
}

template <typename Sample>
NEOTREE_INLINE void run_layer_any_blend(const Layer &layer, float base_alpha, const LedGeometry &geometry, std::span<Rgb> out,
                         uint32_t &evals, Sample sample)
{
    switch (layer.blend)
    {
    case Blend::normal: run_layer<Blend::normal>(layer, base_alpha, geometry, out, evals, sample); break;
    case Blend::add: run_layer<Blend::add>(layer, base_alpha, geometry, out, evals, sample); break;
    case Blend::max: run_layer<Blend::max>(layer, base_alpha, geometry, out, evals, sample); break;
    case Blend::multiply: run_layer<Blend::multiply>(layer, base_alpha, geometry, out, evals, sample); break;
    case Blend::replace: run_layer<Blend::replace>(layer, base_alpha, geometry, out, evals, sample); break;
    }
}

NEOTREE_HOT void composite_layer(const Scene &scene, uint8_t slot, uint8_t layer_index, float slot_opacity,
                                 const LedGeometry &geometry, int64_t time_us, const EntityPool &entities,
                                 EntityScratch &scratch, std::span<Rgb> out, uint32_t &evals)
{
    const Layer &layer = *scene.layer(slot, layer_index);
    const float base_alpha = layer.opacity * slot_opacity;
    if (base_alpha <= 0.0f)
    {
        return;
    }
    switch (layer.type)
    {
    case LayerType::solid:
    {
        const Rgb color = layer.color;
        run_layer_any_blend(layer, base_alpha, geometry, out, evals, [color](size_t, Rgb &src, float &) {
            src = color;
            return true;
        });
        break;
    }
    case LayerType::pixel:
    {
        const Rgba8 *pixels = scene.pixels(layer).data();
        run_layer_any_blend(layer, base_alpha, geometry, out, evals, [pixels](size_t i, Rgb &src, float &a) {
            const Rgba8 px = pixels[i];
            if (px.a == 0)
            {
                return false;
            }
            if (px.a != 255)
            {
                a *= px.a * byte_to_unit;
            }
            src = to_rgb(px.r, px.g, px.b);
            return true;
        });
        break;
    }
    case LayerType::field:
    {
        const FieldParams f = layer.field;
        const LedGeometry *g = &geometry;
        if (f.kind == FieldKind::height_gradient)
        {
            run_layer_any_blend(layer, base_alpha, geometry, out, evals, [f, g](size_t i, Rgb &src, float &) {
                uint16_t led = static_cast<uint16_t>(i);
                if (!g->has_position(led))
                {
                    return false;
                }
                src = lerp(f.color_a, f.color_b, g->height01(led));
                return true;
            });
        }
        else
        {
            const float spin = cycle_phase(static_cast<double>(f.spin_rps), time_us);
            run_layer_any_blend(layer, base_alpha, geometry, out, evals, [f, g, spin](size_t i, Rgb &src, float &) {
                uint16_t led = static_cast<uint16_t>(i);
                if (!g->has_position(led))
                {
                    return false;
                }
                float turn = g->angle(led) / two_pi - spin;
                src = hsv(360.0f * f.hue_cycles * turn, f.saturation, f.value);
                return true;
            });
        }
        break;
    }
    case LayerType::entity:
    {
        evals += render_entities(entities, slot, layer_index, layer.combine, geometry, scratch);
        const EntityScratch *s = &scratch;
        run_layer_any_blend(layer, base_alpha, geometry, out, evals, [s](size_t i, Rgb &src, float &a) {
            const float cover = s->alpha[i];
            if (cover <= 0.0f)
            {
                return false;
            }
            // Premultiplied -> straight color at that coverage.
            const float inv = 1.0f / cover;
            src = {s->color[i].r * inv, s->color[i].g * inv, s->color[i].b * inv};
            a *= cover;
            return true;
        });
        break;
    }
    case LayerType::empty:
        break;
    }
}

}  // namespace

namespace {

// True if the layer paints every LED fully, hiding everything below it.
bool covers_everything(const Slot &slot, const Layer &layer)
{
    return slot.enabled && layer.enabled && layer.type == LayerType::solid && slot.opacity >= 1.0f &&
           layer.opacity >= 1.0f && layer.mask.shape == MaskShape::none &&
           (layer.blend == Blend::normal || layer.blend == Blend::replace);
}

}  // namespace

uint32_t composite(const Scene &scene, const LedGeometry &geometry, int64_t time_us, const EntityPool &entities,
                   EntityScratch &scratch, std::span<Rgb> out)
{
    for (Rgb &px : out)
    {
        px = Rgb{};
    }
    uint32_t evals = 0;
    std::span<Rgb> leds = out.first(out.size() < geometry.count() ? out.size() : geometry.count());

    // Start from the topmost layer that hides everything below it (e.g. a
    // demo's opaque backdrop over the Canvas); nothing under it is visible.
    uint8_t first_slot = 0, first_layer = 0;
    for (int s = max_slots - 1; s >= 0 && first_slot == 0 && first_layer == 0; s--)
    {
        const Slot *slot = scene.slot(static_cast<uint8_t>(s));
        for (int l = slot->layer_count - 1; l >= 0; l--)
        {
            if (covers_everything(*slot, slot->layers[l]))
            {
                first_slot = static_cast<uint8_t>(s);
                first_layer = static_cast<uint8_t>(l);
                break;
            }
        }
    }

    for (uint8_t s = first_slot; s < max_slots; s++)
    {
        const Slot *slot = scene.slot(s);
        if (!slot->enabled || slot->opacity <= 0.0f)
        {
            continue;
        }
        for (uint8_t l = s == first_slot ? first_layer : 0; l < slot->layer_count; l++)
        {
            const Layer &layer = slot->layers[l];
            if (layer.enabled && layer.type != LayerType::empty)
            {
                composite_layer(scene, s, l, slot->opacity, geometry, time_us, entities, scratch, leds, evals);
            }
        }
    }
    return evals;
}

}  // namespace neotree
