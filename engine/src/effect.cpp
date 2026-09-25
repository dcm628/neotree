#include "neotree/effect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "json.hpp"
#include "neotree/engine.hpp"

namespace neotree {

namespace {

using detail::Json;

constexpr float deg_per_rad = 180.0f / pi;

// ---- the schema ----
// Field numbers are their positions: append only.

constexpr FieldType N = FieldType::number;
constexpr FieldType C = FieldType::color;
constexpr FieldType CH = FieldType::choice;
constexpr FieldType T = FieldType::toggle;

constexpr const char *groups_az = "A|B|C|D|E|F|G|H";
constexpr const char *groups_any = "any|A|B|C|D|E|F|G|H";

const EffectField settings_fields[] = {
    {"quota", "Most things at once", N, 1, 250, 1},
    {"forces", "Sets the forces", T},
    {"gravity", "Gravity (mm/s2, down)", N, 0, 20000, 100, nullptr, 1, 0b10},
    {"wind_x", "Wind east (mm/s2)", N, -3000, 3000, 50, nullptr, 1, 0b10},
    {"wind_y", "Wind north (mm/s2)", N, -3000, 3000, 50, nullptr, 1, 0b10},
    {"swirl", "Swirl (turns/s)", N, -2, 2, 0.05f, nullptr, 1, 0b10},
    {"swirl_pull", "Swirl strength", N, 0, 10, 0.1f, nullptr, 1, 0b10},
};

enum LayerField : uint8_t
{
    lf_kind, lf_color, lf_bottom, lf_top, lf_spin, lf_bands, lf_saturation, lf_brightness, lf_combine, lf_blend,
    lf_opacity, lf_mask, lf_band_lo, lf_band_hi, lf_slice_from, lf_slice_to, lf_feather, lf_invert,
};
const EffectField layer_fields[] = {
    {"kind", "Kind", CH, 0, 3, 1, "solid color|gradient|rainbow|things"},
    {"color", "Color", C, 0, 1, 0, nullptr, lf_kind, 0b0001},
    {"bottom", "Bottom color", C, 0, 1, 0, nullptr, lf_kind, 0b0010},
    {"top", "Top color", C, 0, 1, 0, nullptr, lf_kind, 0b0010},
    {"spin", "Spin (turns/s)", N, -2, 2, 0.05f, nullptr, lf_kind, 0b0100},
    {"bands", "Rainbows around", N, 0.25f, 6, 0.25f, nullptr, lf_kind, 0b0100},
    {"saturation", "Saturation", N, 0, 1, 0.05f, nullptr, lf_kind, 0b0100},
    {"brightness", "Brightness", N, 0, 1, 0.05f, nullptr, lf_kind, 0b0100},
    {"combine", "Overlapping things", CH, 0, 1, 1, "add up|brightest", lf_kind, 0b1000},
    {"blend", "Blend", CH, 0, 4, 1, "over|add|brightest|multiply|replace"},
    {"opacity", "Opacity", N, 0, 1, 0.05f},
    {"mask", "Where", CH, 0, 2, 1, "everywhere|a height band|a slice around"},
    {"band_lo", "Band from (mm up)", N, 0, 3000, 10, nullptr, lf_mask, 0b010},
    {"band_hi", "Band to (mm up)", N, 0, 3000, 10, nullptr, lf_mask, 0b010},
    {"slice_from", "Slice from (deg)", N, 0, 360, 1, nullptr, lf_mask, 0b100},
    {"slice_to", "Slice to (deg)", N, 0, 360, 1, nullptr, lf_mask, 0b100},
    {"feather", "Soft edge (mm)", N, 0, 500, 5, nullptr, lf_mask, 0b110},
    {"invert", "Flip it", T, 0, 1, 1, nullptr, lf_mask, 0b110},
};

enum ThingField : uint8_t
{
    tf_shape, tf_size, tf_width, tf_thickness, tf_length, tf_color, tf_brightness, tf_falloff, tf_edge, tf_layer,
    tf_lifetime, tf_fade_in, tf_fade_out, tf_gravity, tf_wind, tf_swirl, tf_drag, tf_vel_x, tf_vel_y, tf_vel_z,
    tf_floor, tf_ceiling, tf_outer, tf_bounce, tf_mass, tf_group, tf_surface, tf_angle, tf_spin, tf_align,
};
const EffectField thing_fields[] = {
    {"shape", "Shape", CH, 0, 4, 1, "ball|slab|bubble|capsule|wedge"},
    {"size", "Size (mm)", N, 5, 1000, 5, nullptr, tf_shape, 0b01111},
    {"width", "Width (deg)", N, 1, 180, 1, nullptr, tf_shape, 0b10000},
    {"thickness", "Thickness (mm)", N, 1, 300, 1, nullptr, tf_shape, 0b00100},
    {"length", "Length (mm)", N, 0, 2000, 10, nullptr, tf_shape, 0b01000},
    {"color", "Color", C},
    {"brightness", "Brightness", N, 0, 3, 0.05f},
    {"falloff", "Edge", CH, 0, 3, 1, "hard|linear|smooth|glow"},
    {"edge", "Edge width (mm)", N, 0, 500, 5, nullptr, tf_falloff, 0b1110},
    {"layer", "Drawn on layer", N, 1, 6, 1},
    {"lifetime", "Lives (s, 0 = forever)", N, 0, 120, 0.1f},
    {"fade_in", "Fades in (s)", N, 0, 10, 0.1f},
    {"fade_out", "Fades out (s)", N, 0, 10, 0.1f},
    {"gravity", "Falls (gravity x)", N, -2, 2, 0.01f},
    {"wind", "Blown by wind", N, 0, 3, 0.05f},
    {"swirl", "Swirled", N, 0, 3, 0.05f},
    {"drag", "Air drag", N, 0, 5, 0.05f},
    {"vel_x", "Moving east (mm/s)", N, -6000, 6000, 10},
    {"vel_y", "Moving north (mm/s)", N, -6000, 6000, 10},
    {"vel_z", "Moving up (mm/s)", N, -6000, 6000, 10},
    {"floor", "At the bottom", CH, 0, 5, 1, "passes|stops|bounces|vanishes|starts over|wraps to the top"},
    {"ceiling", "At the top", CH, 0, 5, 1, "passes|stops|bounces|vanishes|starts over|wraps to the bottom"},
    {"outer", "At the outside", CH, 0, 4, 1, "passes|stops|bounces|vanishes|starts over"},
    {"bounce", "Bounciness", N, 0, 1.2f, 0.05f},
    {"mass", "Weight", N, 0.1f, 10, 0.1f},
    {"group", "Group", CH, 0, 8, 1, "none|A|B|C|D|E|F|G|H"},
    {"surface", "Stays on the surface", T},
    {"angle", "Angle (deg)", N, 0, 360, 1, nullptr, tf_shape, 0b10000},
    {"spin", "Spin (turns/s)", N, -3, 3, 0.05f, nullptr, tf_shape, 0b10000},
    {"align", "Points along its motion", T, 0, 1, 1, nullptr, tf_shape, 0b01010},
};

enum SourceField : uint8_t
{
    sf_thing, sf_rate, sf_on, sf_region, sf_from_x, sf_from_y, sf_from_z, sf_to_x, sf_to_y, sf_to_z, sf_r_from,
    sf_r_to, sf_vel_x, sf_vel_y, sf_vel_z, sf_spread, sf_size_jitter, sf_life_jitter, sf_color_from, sf_color,
};
const EffectField source_fields[] = {
    {"thing", "Makes thing", N, 1, 8, 1},
    {"rate", "Per second", N, 0, 50, 0.1f},
    {"on", "On", T},
    {"region", "From", CH, 0, 4, 1, "one point|a box|a band around|the top|anywhere inside"},
    {"from_x", "At x (mm)", N, -2000, 2000, 10, nullptr, sf_region, 0b00011},
    {"from_y", "At y (mm)", N, -2000, 2000, 10, nullptr, sf_region, 0b00011},
    {"from_z", "At height (mm)", N, 0, 3000, 10, nullptr, sf_region, 0b00111},
    {"to_x", "To x (mm)", N, -2000, 2000, 10, nullptr, sf_region, 0b00010},
    {"to_y", "To y (mm)", N, -2000, 2000, 10, nullptr, sf_region, 0b00010},
    {"to_z", "To height (mm)", N, 0, 3000, 10, nullptr, sf_region, 0b00110},
    {"r_from", "Radius from (mm)", N, 0, 1500, 10, nullptr, sf_region, 0b00100},
    {"r_to", "Radius to (mm)", N, 0, 1500, 10, nullptr, sf_region, 0b00100},
    {"vel_x", "Push east (mm/s)", N, -6000, 6000, 10},
    {"vel_y", "Push north (mm/s)", N, -6000, 6000, 10},
    {"vel_z", "Push up (mm/s)", N, -6000, 6000, 10},
    {"spread", "Random speed (mm/s)", N, 0, 5000, 10},
    {"size_jitter", "Size varies (+-)", N, 0, 1, 0.05f},
    {"life_jitter", "Life varies (+-)", N, 0, 1, 0.05f},
    {"color_from", "Color", CH, 0, 2, 1, "the thing's|this one|random"},
    {"color", "Color", C, 0, 1, 0, nullptr, sf_color_from, 0b010},
};

const EffectField meet_fields[] = {
    {"a", "Group", CH, 0, 7, 1, groups_az},
    {"b", "Meets group", CH, 0, 7, 1, groups_az},
    {"what", "What happens", CH, 0, 6, 1,
     "nothing|pass through|bounce|stick together|the first vanishes|the second vanishes|both vanish"},
};

enum RuleField : uint8_t
{
    rf_on, rf_when, rf_group_a, rf_group_b, rf_edge, rf_id, rf_delay, rf_period, rf_threshold, rf_chance,
    rf_cooldown, rf_max_fires, rf_limit_group, rf_limit_count,
};
const EffectField rule_fields[] = {
    {"on", "On", T},
    {"when", "When", CH, 0, 10, 1,
     "things meet|they start to overlap|they stop overlapping|a thing hits an edge|a thing's life ends|"
     "a thing appears|a timer goes off|a signal comes|a button is pressed|a group drops below|a group rises above"},
    {"group_a", "Group", CH, 0, 8, 1, groups_any, rf_when, 0b11000111111},
    {"group_b", "And group", CH, 0, 8, 1, groups_any, rf_when, 0b111},
    {"edge", "Which edge", CH, 0, 3, 1, "any|the bottom|the top|the outside", rf_when, 0b1000},
    {"id", "Number", N, 0, 255, 1, nullptr, rf_when, 0b110000000},
    {"delay", "After (s)", N, 0, 600, 0.1f, nullptr, rf_when, 0b1000000},
    {"period", "Then every (s, 0 = once)", N, 0, 600, 0.1f, nullptr, rf_when, 0b1000000},
    {"threshold", "Count", N, 0, 250, 1, nullptr, rf_when, 0b11000000000},
    {"chance", "Chance", N, 0, 1, 0.01f},
    {"cooldown", "Not again for (s)", N, 0, 60, 0.05f},
    {"max_fires", "At most (times, 0 = no limit)", N, 0, 1000, 1},
    {"limit_group", "Only while group", CH, 0, 8, 1, groups_any},
    {"limit_count", "has fewer than", N, 0, 250, 1, nullptr, rf_limit_group, 0b111111110},
};

enum ActionField : uint8_t
{
    af_do, af_target, af_group, af_thing, af_source, af_layer, af_signal, af_outcome, af_count, af_place,
    af_color_from, af_color, af_vel_x, af_vel_y, af_vel_z, af_inherit, af_spread, af_value, af_at_x, af_at_y,
    af_at_z,
};
const EffectField action_fields[] = {
    {"do", "Do", CH, 0, 14, 1,
     "make things|remove|recolor|set speed|push|turn a source on|turn a source off|set a source's rate|"
     "set gravity|set wind|recolor a layer|fade a layer|send a signal|count a cycle|end the mode"},
    {"target", "Which", CH, 0, 3, 1, "the first|the second|both|a whole group", af_do, 0b11110},
    {"group", "Group", CH, 0, 7, 1, groups_az, af_target, 0b1000},
    {"thing", "Thing", N, 1, 8, 1, nullptr, af_do, 0b1},
    {"source", "Source", N, 1, 8, 1, nullptr, af_do, 0b11100000},
    {"layer", "Layer", N, 1, 6, 1, nullptr, af_do, 0b110000000000},
    {"signal", "Signal number", N, 0, 255, 1, nullptr, af_do, 0b1000000000000},
    {"outcome", "Outcome", N, 0, 255, 1, nullptr, af_do, 0b100000000000000},
    {"count", "How many", N, 1, 50, 1, nullptr, af_do, 0b1},
    {"place", "Where", CH, 0, 4, 1, "where it happened|at the first|at the second|a fixed spot|anywhere inside",
     af_do, 0b1},
    {"color_from", "Color", CH, 0, 6, 1,
     "keep|this one|the first's|the second's|a mix|random (all the same)|random each", af_do, 0b10000000101},
    {"color", "Color", C, 0, 1, 0, nullptr, af_color_from, 0b10},
    {"vel_x", "East (mm/s)", N, -6000, 6000, 10, nullptr, af_do, 0b1100011001},
    {"vel_y", "North (mm/s)", N, -6000, 6000, 10, nullptr, af_do, 0b1100011001},
    {"vel_z", "Up (mm/s)", N, -20000, 20000, 10, nullptr, af_do, 0b1100011001},
    {"inherit", "Keeps its speed", N, 0, 1, 0.05f, nullptr, af_do, 0b1},
    {"spread", "Scatter (mm/s)", N, 0, 5000, 10, nullptr, af_do, 0b1},
    {"value", "Rate / opacity", N, 0, 50, 0.05f, nullptr, af_do, 0b100010000000},
    {"at_x", "Spot x (mm)", N, -2000, 2000, 10, nullptr, af_place, 0b1000},
    {"at_y", "Spot y (mm)", N, -2000, 2000, 10, nullptr, af_place, 0b1000},
    {"at_z", "Spot height (mm)", N, 0, 3000, 10, nullptr, af_place, 0b1000},
};

const EffectField start_fields[] = {
    {"thing", "Thing", N, 1, 8, 1},
    {"count", "How many", N, 0, 100, 1},
    {"place", "Where", CH, 0, 2, 1, "anywhere inside|at the top|at the bottom"},
    {"speed", "Random speed (mm/s)", N, 0, 5000, 10},
    {"colors", "Colors", CH, 0, 1, 1, "the thing's|random"},
};

#define FIELDS(f) f, static_cast<uint8_t>(sizeof(f) / sizeof(f[0]))
const EffectSectionInfo sections[] = {
    {"settings", "Settings", "Settings", FIELDS(settings_fields), 1},
    {"layers", "Backgrounds", "Layer", FIELDS(layer_fields), max_layers_per_slot},
    {"things", "Things", "Thing", FIELDS(thing_fields), max_templates_per_slot},
    {"sources", "Sources", "Source", FIELDS(source_fields), max_emitters_per_slot},
    {"meets", "When things meet", "Meeting", FIELDS(meet_fields), max_effect_meets},
    {"rules", "Rules", "Rule", FIELDS(rule_fields), max_effect_rules},
    {"actions", "Then", "Action", FIELDS(action_fields), max_actions_per_rule},
    {"starts", "Start with", "Start", FIELDS(start_fields), max_effect_starts},
};
#undef FIELDS

// ---- enum <-> choice helpers ----

uint8_t group_to_choice_any(uint8_t g) { return g >= max_groups ? 0 : static_cast<uint8_t>(g + 1); }
uint8_t choice_any_to_group(float v, uint8_t none)
{
    const int k = static_cast<int>(v + 0.5f);
    return k <= 0 ? none : static_cast<uint8_t>(std::min(k - 1, static_cast<int>(max_groups) - 1));
}
int as_int(float v) { return static_cast<int>(std::lround(v)); }
uint8_t as_u8(float v, int lo, int hi) { return static_cast<uint8_t>(std::clamp(as_int(v), lo, hi)); }

// ---- per-section get / set ----

bool get_settings(const Effect &e, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case 0: v.f = e.quota; return true;
    case 1: v.f = e.sets_forces ? 1.0f : 0.0f; return true;
    case 2: v.f = -e.forces.gravity.z; return true;
    case 3: v.f = e.forces.wind.x; return true;
    case 4: v.f = e.forces.wind.y; return true;
    case 5: v.f = e.forces.swirl_rad_s / two_pi; return true;
    case 6: v.f = e.forces.swirl_rate; return true;
    default: return false;
    }
}

bool set_settings(Effect &e, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case 0: e.quota = static_cast<uint16_t>(std::clamp(as_int(v.f), 1, 250)); return true;
    case 1: e.sets_forces = v.f >= 0.5f; return true;
    case 2: e.forces.gravity = {0.0f, 0.0f, -v.f}; return true;
    case 3: e.forces.wind.x = v.f; return true;
    case 4: e.forces.wind.y = v.f; return true;
    case 5: e.forces.swirl_rad_s = v.f * two_pi; return true;
    case 6: e.forces.swirl_rate = v.f; return true;
    default: return false;
    }
}

int layer_kind(const Layer &l)
{
    switch (l.type)
    {
    case LayerType::field: return l.field.kind == FieldKind::angle_rainbow ? 2 : 1;
    case LayerType::entity: return 3;
    default: return 0;
    }
}

int mask_kind(const Mask &m)
{
    if (m.shape != MaskShape::cylinder)
    {
        return 0;
    }
    const bool full_circle = m.angle_min <= 0.0f && m.angle_max >= two_pi - 1e-4f;
    return full_circle ? 1 : 2;
}

bool get_layer(const Layer &l, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case lf_kind: v.f = static_cast<float>(layer_kind(l)); return true;
    case lf_color: v.c = l.color; return true;
    case lf_bottom: v.c = l.field.color_a; return true;
    case lf_top: v.c = l.field.color_b; return true;
    case lf_spin: v.f = l.field.spin_rps; return true;
    case lf_bands: v.f = l.field.hue_cycles; return true;
    case lf_saturation: v.f = l.field.saturation; return true;
    case lf_brightness: v.f = l.field.value; return true;
    case lf_combine: v.f = l.combine == EntityCombine::max ? 1.0f : 0.0f; return true;
    case lf_blend: v.f = static_cast<float>(l.blend); return true;
    case lf_opacity: v.f = l.opacity; return true;
    case lf_mask: v.f = static_cast<float>(mask_kind(l.mask)); return true;
    case lf_band_lo: v.f = mask_kind(l.mask) == 1 ? l.mask.z_min : 0.0f; return true;
    case lf_band_hi: v.f = mask_kind(l.mask) == 1 ? l.mask.z_max : 1000.0f; return true;
    case lf_slice_from: v.f = mask_kind(l.mask) == 2 ? l.mask.angle_min * deg_per_rad : 0.0f; return true;
    case lf_slice_to: v.f = mask_kind(l.mask) == 2 ? l.mask.angle_max * deg_per_rad : 90.0f; return true;
    case lf_feather: v.f = l.mask.feather_mm; return true;
    case lf_invert: v.f = l.mask.invert ? 1.0f : 0.0f; return true;
    default: return false;
    }
}

bool set_layer(Layer &l, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case lf_kind:
        switch (as_int(v.f))
        {
        case 1: l.type = LayerType::field; l.field.kind = FieldKind::height_gradient; break;
        case 2: l.type = LayerType::field; l.field.kind = FieldKind::angle_rainbow; break;
        case 3: l.type = LayerType::entity; break;
        default: l.type = LayerType::solid; break;
        }
        return true;
    case lf_color: l.color = v.c; return true;
    case lf_bottom: l.field.color_a = v.c; return true;
    case lf_top: l.field.color_b = v.c; return true;
    case lf_spin: l.field.spin_rps = v.f; return true;
    case lf_bands: l.field.hue_cycles = v.f; return true;
    case lf_saturation: l.field.saturation = clamp01(v.f); return true;
    case lf_brightness: l.field.value = clamp01(v.f); return true;
    case lf_combine: l.combine = v.f >= 0.5f ? EntityCombine::max : EntityCombine::add; return true;
    case lf_blend: l.blend = static_cast<Blend>(as_u8(v.f, 0, 4)); return true;
    case lf_opacity: l.opacity = clamp01(v.f); return true;
    case lf_mask:
    {
        const int kind = as_int(v.f);
        if (kind == 1 && mask_kind(l.mask) != 1)
        {
            l.mask = Mask::make_cylinder(0.0f, 1000.0f, 0.0f, 1.0e9f, 0.0f, two_pi, l.mask.feather_mm);
        }
        else if (kind == 2 && mask_kind(l.mask) != 2)
        {
            l.mask = Mask::make_cylinder(-1.0e6f, 1.0e6f, 0.0f, 1.0e9f, 0.0f, 90.0f / deg_per_rad, l.mask.feather_mm);
        }
        else if (kind == 0)
        {
            l.mask.shape = MaskShape::none;
        }
        return true;
    }
    case lf_band_lo: l.mask.z_min = v.f; return true;
    case lf_band_hi: l.mask.z_max = v.f; return true;
    case lf_slice_from: l.mask.angle_min = std::clamp(v.f, 0.0f, 359.9f) / deg_per_rad; return true;
    case lf_slice_to: l.mask.angle_max = std::clamp(v.f, 0.0f, 360.0f) / deg_per_rad; return true;
    case lf_feather: l.mask.feather_mm = std::max(v.f, 0.0f); return true;
    case lf_invert: l.mask.invert = v.f >= 0.5f; return true;
    default: return false;
    }
}

bool get_thing(const Entity &e, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case tf_shape: v.f = static_cast<float>(e.shape); return true;
    case tf_size: v.f = e.size; return true;
    case tf_width: v.f = 2.0f * e.size * deg_per_rad; return true;
    case tf_thickness: v.f = e.thickness; return true;
    case tf_length: v.f = e.length; return true;
    case tf_color: v.c = e.color; return true;
    case tf_brightness: v.f = e.brightness; return true;
    case tf_falloff: v.f = static_cast<float>(e.falloff); return true;
    case tf_edge: v.f = e.edge_mm; return true;
    case tf_layer: v.f = static_cast<float>(e.layer + 1); return true;
    case tf_lifetime: v.f = e.lifetime_s; return true;
    case tf_fade_in: v.f = e.fade_in_s; return true;
    case tf_fade_out: v.f = e.fade_out_s; return true;
    case tf_gravity: v.f = e.gravity_scale; return true;
    case tf_wind: v.f = e.wind_scale; return true;
    case tf_swirl: v.f = e.swirl_scale; return true;
    case tf_drag: v.f = e.drag; return true;
    case tf_vel_x: v.f = e.vel.x; return true;
    case tf_vel_y: v.f = e.vel.y; return true;
    case tf_vel_z: v.f = e.vel.z; return true;
    case tf_floor: v.f = static_cast<float>(e.floor); return true;
    case tf_ceiling: v.f = static_cast<float>(e.ceiling); return true;
    case tf_outer: v.f = static_cast<float>(e.outer == Bound::wrap ? Bound::stop : e.outer); return true;
    case tf_bounce: v.f = e.restitution; return true;
    case tf_mass: v.f = e.mass; return true;
    case tf_group: v.f = static_cast<float>(group_to_choice_any(e.group)); return true;
    case tf_surface: v.f = e.surface == Surface::envelope ? 1.0f : 0.0f; return true;
    case tf_angle: v.f = e.angle * deg_per_rad; return true;
    case tf_spin: v.f = e.spin / two_pi; return true;
    case tf_align: v.f = e.align_to_velocity ? 1.0f : 0.0f; return true;
    default: return false;
    }
}

bool set_thing(Entity &e, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case tf_shape: e.shape = static_cast<Shape>(as_u8(v.f, 0, 4)); return true;
    case tf_size: e.size = std::max(v.f, 0.0f); return true;
    case tf_width: e.size = std::max(v.f, 0.0f) * 0.5f / deg_per_rad; return true;
    case tf_thickness: e.thickness = std::max(v.f, 0.0f); return true;
    case tf_length: e.length = std::max(v.f, 0.0f); return true;
    case tf_color: e.color = v.c; return true;
    case tf_brightness: e.brightness = std::max(v.f, 0.0f); return true;
    case tf_falloff: e.falloff = static_cast<Falloff>(as_u8(v.f, 0, 3)); return true;
    case tf_edge: e.edge_mm = std::max(v.f, 0.0f); return true;
    case tf_layer: e.layer = as_u8(v.f - 1.0f, 0, max_layers_per_slot - 1); return true;
    case tf_lifetime: e.lifetime_s = std::max(v.f, 0.0f); return true;
    case tf_fade_in: e.fade_in_s = std::max(v.f, 0.0f); return true;
    case tf_fade_out: e.fade_out_s = std::max(v.f, 0.0f); return true;
    case tf_gravity: e.gravity_scale = v.f; return true;
    case tf_wind: e.wind_scale = v.f; return true;
    case tf_swirl: e.swirl_scale = v.f; return true;
    case tf_drag: e.drag = std::max(v.f, 0.0f); return true;
    case tf_vel_x: e.vel.x = v.f; return true;
    case tf_vel_y: e.vel.y = v.f; return true;
    case tf_vel_z: e.vel.z = v.f; return true;
    case tf_floor: e.floor = static_cast<Bound>(as_u8(v.f, 0, 5)); return true;
    case tf_ceiling: e.ceiling = static_cast<Bound>(as_u8(v.f, 0, 5)); return true;
    case tf_outer: e.outer = static_cast<Bound>(as_u8(v.f, 0, 4)); return true;
    case tf_bounce: e.restitution = std::max(v.f, 0.0f); return true;
    case tf_mass: e.mass = std::max(v.f, 0.01f); return true;
    case tf_group: e.group = choice_any_to_group(v.f, no_group); return true;
    case tf_surface: e.surface = v.f >= 0.5f ? Surface::envelope : Surface::none; return true;
    case tf_angle: e.angle = std::fmod(std::max(v.f, 0.0f), 360.0f) / deg_per_rad; return true;
    case tf_spin: e.spin = v.f * two_pi; return true;
    case tf_align: e.align_to_velocity = v.f >= 0.5f; return true;
    default: return false;
    }
}

int region_choice(Region r)
{
    switch (r)
    {
    case Region::point: return 0;
    case Region::box: return 1;
    case Region::band: return 2;
    case Region::top: return 3;
    default: return 4;   // volume (and riding an entity, which an effect can't express)
    }
}

bool get_source(const Emitter &m, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case sf_thing: v.f = static_cast<float>(m.template_index + 1); return true;
    case sf_rate: v.f = m.rate; return true;
    case sf_on: v.f = m.active ? 1.0f : 0.0f; return true;
    case sf_region: v.f = static_cast<float>(region_choice(m.region)); return true;
    case sf_from_x: case sf_r_from: v.f = m.min.x; return true;
    case sf_from_y: v.f = m.min.y; return true;
    case sf_from_z: v.f = m.min.z; return true;
    case sf_to_x: case sf_r_to: v.f = m.max.x; return true;
    case sf_to_y: v.f = m.max.y; return true;
    case sf_to_z: v.f = m.max.z; return true;
    case sf_vel_x: v.f = m.vel.x; return true;
    case sf_vel_y: v.f = m.vel.y; return true;
    case sf_vel_z: v.f = m.vel.z; return true;
    case sf_spread: v.f = m.spread; return true;
    case sf_size_jitter: v.f = m.size_jitter; return true;
    case sf_life_jitter: v.f = m.lifetime_jitter; return true;
    case sf_color_from:
        v.f = m.color_from == ColorFrom::fixed ? 1.0f : m.color_from == ColorFrom::keep ? 0.0f : 2.0f;
        return true;
    case sf_color: v.c = m.color; return true;
    default: return false;
    }
}

bool set_source(Emitter &m, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case sf_thing: m.template_index = as_u8(v.f - 1.0f, 0, max_templates_per_slot - 1); return true;
    case sf_rate: m.rate = std::max(v.f, 0.0f); return true;
    case sf_on: m.active = v.f >= 0.5f; return true;
    case sf_region:
    {
        static const Region regions[] = {Region::point, Region::box, Region::band, Region::top, Region::volume};
        m.region = regions[as_u8(v.f, 0, 4)];
        return true;
    }
    case sf_from_x: case sf_r_from: m.min.x = v.f; return true;
    case sf_from_y: m.min.y = v.f; return true;
    case sf_from_z: m.min.z = v.f; return true;
    case sf_to_x: case sf_r_to: m.max.x = v.f; return true;
    case sf_to_y: m.max.y = v.f; return true;
    case sf_to_z: m.max.z = v.f; return true;
    case sf_vel_x: m.vel.x = v.f; return true;
    case sf_vel_y: m.vel.y = v.f; return true;
    case sf_vel_z: m.vel.z = v.f; return true;
    case sf_spread: m.spread = std::max(v.f, 0.0f); return true;
    case sf_size_jitter: m.size_jitter = clamp01(v.f); return true;
    case sf_life_jitter: m.lifetime_jitter = clamp01(v.f); return true;
    case sf_color_from:
    {
        const int k = as_int(v.f);
        m.color_from = k == 1 ? ColorFrom::fixed : k == 2 ? ColorFrom::random_each : ColorFrom::keep;
        return true;
    }
    case sf_color: m.color = v.c; return true;
    default: return false;
    }
}

bool get_meet(const Meet &m, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case 0: v.f = m.a; return true;
    case 1: v.f = m.b; return true;
    case 2: v.f = static_cast<float>(m.response); return true;
    default: return false;
    }
}

bool set_meet(Meet &m, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case 0: m.a = as_u8(v.f, 0, max_groups - 1); return true;
    case 1: m.b = as_u8(v.f, 0, max_groups - 1); return true;
    case 2: m.response = static_cast<Response>(as_u8(v.f, 0, 6)); return true;
    default: return false;
    }
}

int edge_choice(uint8_t id)
{
    switch (id)
    {
    case hit_floor: return 1;
    case hit_ceiling: return 2;
    case hit_outer: return 3;
    default: return 0;
    }
}

bool get_rule(const Rule &r, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case rf_on: v.f = r.enabled ? 1.0f : 0.0f; return true;
    case rf_when: v.f = static_cast<float>(r.trigger); return true;
    case rf_group_a: v.f = static_cast<float>(group_to_choice_any(r.group_a)); return true;
    case rf_group_b: v.f = static_cast<float>(group_to_choice_any(r.group_b)); return true;
    case rf_edge: v.f = static_cast<float>(edge_choice(r.id)); return true;
    case rf_id: v.f = r.id; return true;
    case rf_delay: v.f = r.delay_s; return true;
    case rf_period: v.f = r.period_s; return true;
    case rf_threshold: v.f = r.threshold; return true;
    case rf_chance: v.f = r.probability; return true;
    case rf_cooldown: v.f = r.cooldown_s; return true;
    case rf_max_fires: v.f = r.max_fires; return true;
    case rf_limit_group: v.f = static_cast<float>(group_to_choice_any(r.limit_group)); return true;
    case rf_limit_count: v.f = r.limit_count; return true;
    default: return false;
    }
}

bool set_rule(Rule &r, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case rf_on: r.enabled = v.f >= 0.5f; return true;
    case rf_when: r.trigger = static_cast<Trigger>(as_u8(v.f, 0, 10)); return true;
    case rf_group_a: r.group_a = choice_any_to_group(v.f, any_group); return true;
    case rf_group_b: r.group_b = choice_any_to_group(v.f, any_group); return true;
    case rf_edge:
    {
        static const uint8_t ids[] = {0, hit_floor, hit_ceiling, hit_outer};
        r.id = ids[as_u8(v.f, 0, 3)];
        return true;
    }
    case rf_id: r.id = as_u8(v.f, 0, 255); return true;
    case rf_delay: r.delay_s = std::max(v.f, 0.0f); return true;
    case rf_period: r.period_s = std::max(v.f, 0.0f); return true;
    case rf_threshold: r.threshold = static_cast<uint16_t>(std::clamp(as_int(v.f), 0, 1000)); return true;
    case rf_chance: r.probability = clamp01(v.f); return true;
    case rf_cooldown: r.cooldown_s = std::max(v.f, 0.0f); return true;
    case rf_max_fires: r.max_fires = static_cast<uint16_t>(std::clamp(as_int(v.f), 0, 60000)); return true;
    case rf_limit_group: r.limit_group = choice_any_to_group(v.f, no_group); return true;
    case rf_limit_count: r.limit_count = static_cast<uint16_t>(std::clamp(as_int(v.f), 0, 1000)); return true;
    default: return false;
    }
}

// Action::index means a thing, source or layer (1-based in the editor), or
// a signal / outcome number, depending on what the action does.
bool get_action(const Action &a, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case af_do: v.f = static_cast<float>(a.type); return true;
    case af_target: v.f = static_cast<float>(a.target); return true;
    case af_group: v.f = a.group; return true;
    case af_thing: case af_source: case af_layer: v.f = static_cast<float>(a.index + 1); return true;
    case af_signal: case af_outcome: v.f = a.index; return true;
    case af_count: v.f = a.count; return true;
    case af_place: v.f = static_cast<float>(a.place); return true;
    case af_color_from: v.f = static_cast<float>(a.color_from); return true;
    case af_color: v.c = a.color; return true;
    case af_vel_x: v.f = a.vel.x; return true;
    case af_vel_y: v.f = a.vel.y; return true;
    case af_vel_z: v.f = a.vel.z; return true;
    case af_inherit: v.f = a.inherit; return true;
    case af_spread: v.f = a.spread; return true;
    case af_value: v.f = a.value; return true;
    case af_at_x: v.f = a.pos.x; return true;
    case af_at_y: v.f = a.pos.y; return true;
    case af_at_z: v.f = a.pos.z; return true;
    default: return false;
    }
}

bool set_action(Action &a, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case af_do: a.type = static_cast<ActionType>(as_u8(v.f, 0, 14)); return true;
    case af_target: a.target = static_cast<Target>(as_u8(v.f, 0, 3)); return true;
    case af_group: a.group = as_u8(v.f, 0, max_groups - 1); return true;
    case af_thing: case af_source: case af_layer: a.index = as_u8(v.f - 1.0f, 0, 15); return true;
    case af_signal: case af_outcome: a.index = as_u8(v.f, 0, 255); return true;
    case af_count: a.count = as_u8(v.f, 1, 50); return true;
    case af_place: a.place = static_cast<Place>(as_u8(v.f, 0, 4)); return true;
    case af_color_from: a.color_from = static_cast<ColorFrom>(as_u8(v.f, 0, 6)); return true;
    case af_color: a.color = v.c; return true;
    case af_vel_x: a.vel.x = v.f; return true;
    case af_vel_y: a.vel.y = v.f; return true;
    case af_vel_z: a.vel.z = v.f; return true;
    case af_inherit: a.inherit = clamp01(v.f); return true;
    case af_spread: a.spread = std::max(v.f, 0.0f); return true;
    case af_value: a.value = v.f; return true;
    case af_at_x: a.pos.x = v.f; return true;
    case af_at_y: a.pos.y = v.f; return true;
    case af_at_z: a.pos.z = v.f; return true;
    default: return false;
    }
}

bool get_start(const Start &s, uint8_t f, FieldValue &v)
{
    switch (f)
    {
    case 0: v.f = static_cast<float>(s.thing + 1); return true;
    case 1: v.f = s.count; return true;
    case 2: v.f = static_cast<float>(s.place); return true;
    case 3: v.f = s.speed; return true;
    case 4: v.f = s.random_colors ? 1.0f : 0.0f; return true;
    default: return false;
    }
}

bool set_start(Start &s, uint8_t f, const FieldValue &v)
{
    switch (f)
    {
    case 0: s.thing = as_u8(v.f - 1.0f, 0, max_templates_per_slot - 1); return true;
    case 1: s.count = as_u8(v.f, 0, 100); return true;
    case 2: s.place = static_cast<StartPlace>(as_u8(v.f, 0, 2)); return true;
    case 3: s.speed = std::max(v.f, 0.0f); return true;
    case 4: s.random_colors = v.f >= 0.5f; return true;
    default: return false;
    }
}

// ---- new items ----

void new_layer(Layer &l)
{
    l = Layer{};
    l.type = LayerType::solid;
    l.color = {0.0f, 0.0f, 0.05f};
}

void new_thing(Entity &e)
{
    e = Entity{};
    e.size = 80.0f;
    e.edge_mm = 60.0f;
    e.gravity_scale = 0.1f;
    e.floor = Bound::bounce;
    e.outer = Bound::bounce;
    e.lifetime_s = 8.0f;
    e.fade_out_s = 2.0f;
    e.group = 0;
}

void new_source(Emitter &m)
{
    m = Emitter{};
    m.rate = 2.0f;
    m.region = Region::top;
    m.spread = 300.0f;
}

void new_rule(Rule &r) { r = Rule{}; }

void new_action(Action &a)
{
    a = Action{};
    a.count = 5;
    a.spread = 800.0f;
}

void new_start(Start &s)
{
    s = Start{};
    s.count = 5;
    s.speed = 300.0f;
}

template <typename Item>
bool remove_at(Item *items, uint8_t &count, uint8_t index)
{
    if (index >= count)
    {
        return false;
    }
    for (uint8_t i = index; i + 1 < count; i++)
    {
        items[i] = items[i + 1];
    }
    count--;
    return true;
}

// Where references point after item `removed` goes: past it, one down.
uint8_t shifted(uint8_t ref, uint8_t removed) { return ref > removed ? static_cast<uint8_t>(ref - 1) : ref; }

void clear_effect(Effect &e)
{
    e.name[0] = '\0';
    e.layer_count = 0;
    e.thing_count = 0;
    e.source_count = 0;
    e.meet_count = 0;
    e.rule_count = 0;
    e.start_count = 0;
    e.quota = default_slot_quota;
    e.sets_forces = false;
    e.forces = Forces{};
}

}  // namespace

const EffectSectionInfo &effect_section(EffectSection section)
{
    const uint8_t s = static_cast<uint8_t>(section);
    return sections[s < static_cast<uint8_t>(EffectSection::count) ? s : 0];
}

uint8_t effect_item_count(const Effect &e, EffectSection section)
{
    switch (section)
    {
    case EffectSection::settings: return 1;
    case EffectSection::layers: return e.layer_count;
    case EffectSection::things: return e.thing_count;
    case EffectSection::sources: return e.source_count;
    case EffectSection::meets: return e.meet_count;
    case EffectSection::rules: return e.rule_count;
    case EffectSection::actions:
    {
        uint8_t n = 0;
        for (uint8_t r = 0; r < e.rule_count; r++)
        {
            n = static_cast<uint8_t>(n + e.rules[r].action_count);
        }
        return n;
    }
    case EffectSection::starts: return e.start_count;
    default: return 0;
    }
}

bool effect_item_exists(const Effect &e, EffectSection section, uint8_t index)
{
    if (section == EffectSection::actions)
    {
        const uint8_t r = index / max_actions_per_rule;
        return r < e.rule_count && index % max_actions_per_rule < e.rules[r].action_count;
    }
    return index < effect_item_count(e, section);
}

bool effect_get(const Effect &e, EffectSection section, uint8_t index, uint8_t field, FieldValue &out)
{
    if (!effect_item_exists(e, section, index))
    {
        return false;
    }
    switch (section)
    {
    case EffectSection::settings: return get_settings(e, field, out);
    case EffectSection::layers: return get_layer(e.layers[index], field, out);
    case EffectSection::things: return get_thing(e.things[index], field, out);
    case EffectSection::sources: return get_source(e.sources[index], field, out);
    case EffectSection::meets: return get_meet(e.meets[index], field, out);
    case EffectSection::rules: return get_rule(e.rules[index], field, out);
    case EffectSection::actions:
        return get_action(e.rules[index / max_actions_per_rule].actions[index % max_actions_per_rule], field, out);
    case EffectSection::starts: return get_start(e.starts[index], field, out);
    default: return false;
    }
}

bool effect_set(Effect &e, EffectSection section, uint8_t index, uint8_t field, const FieldValue &v)
{
    if (!effect_item_exists(e, section, index))
    {
        return false;
    }
    switch (section)
    {
    case EffectSection::settings: return set_settings(e, field, v);
    case EffectSection::layers: return set_layer(e.layers[index], field, v);
    case EffectSection::things: return set_thing(e.things[index], field, v);
    case EffectSection::sources: return set_source(e.sources[index], field, v);
    case EffectSection::meets: return set_meet(e.meets[index], field, v);
    case EffectSection::rules: return set_rule(e.rules[index], field, v);
    case EffectSection::actions:
        return set_action(e.rules[index / max_actions_per_rule].actions[index % max_actions_per_rule], field, v);
    case EffectSection::starts: return set_start(e.starts[index], field, v);
    default: return false;
    }
}

namespace {

// A new default item, as is (what the stored form's fields differ from).
bool add_item(Effect &e, EffectSection section, uint8_t index)
{
    switch (section)
    {
    case EffectSection::layers:
        if (e.layer_count >= max_layers_per_slot) return false;
        new_layer(e.layers[e.layer_count++]);
        return true;
    case EffectSection::things:
    {
        if (e.thing_count >= max_templates_per_slot) return false;
        new_thing(e.things[e.thing_count++]);
        return true;
    }
    case EffectSection::sources:
        if (e.source_count >= max_emitters_per_slot) return false;
        new_source(e.sources[e.source_count++]);
        return true;
    case EffectSection::meets:
        if (e.meet_count >= max_effect_meets) return false;
        e.meets[e.meet_count++] = Meet{};
        return true;
    case EffectSection::rules:
        if (e.rule_count >= max_effect_rules) return false;
        new_rule(e.rules[e.rule_count++]);
        return true;
    case EffectSection::actions:
    {
        if (index >= e.rule_count || e.rules[index].action_count >= max_actions_per_rule) return false;
        Rule &r = e.rules[index];
        new_action(r.actions[r.action_count++]);
        return true;
    }
    case EffectSection::starts:
        if (e.start_count >= max_effect_starts) return false;
        new_start(e.starts[e.start_count++]);
        return true;
    default:
        return false;
    }
}

}  // namespace

bool effect_add(Effect &e, EffectSection section, uint8_t index)
{
    if (!add_item(e, section, index))
    {
        return false;
    }
    if (section == EffectSection::things)
    {
        // Drawn on the first things layer, if there is one.
        Entity &t = e.things[e.thing_count - 1];
        for (uint8_t l = 0; l < e.layer_count; l++)
        {
            if (e.layers[l].type == LayerType::entity)
            {
                t.layer = l;
                break;
            }
        }
    }
    return true;
}

bool effect_remove(Effect &e, EffectSection section, uint8_t index)
{
    switch (section)
    {
    case EffectSection::layers:
        if (!remove_at(e.layers, e.layer_count, index)) return false;
        for (uint8_t t = 0; t < e.thing_count; t++)
        {
            e.things[t].layer = shifted(e.things[t].layer, index);
        }
        return true;
    case EffectSection::things:
        if (!remove_at(e.things, e.thing_count, index)) return false;
        // What referred to the things after it follows them down.
        for (uint8_t s = 0; s < e.source_count; s++)
        {
            e.sources[s].template_index = shifted(e.sources[s].template_index, index);
        }
        for (uint8_t s = 0; s < e.start_count; s++)
        {
            e.starts[s].thing = shifted(e.starts[s].thing, index);
        }
        for (uint8_t r = 0; r < e.rule_count; r++)
        {
            for (uint8_t a = 0; a < e.rules[r].action_count; a++)
            {
                Action &act = e.rules[r].actions[a];
                if (act.type == ActionType::spawn)
                {
                    act.index = shifted(act.index, index);
                }
            }
        }
        return true;
    case EffectSection::sources: return remove_at(e.sources, e.source_count, index);
    case EffectSection::meets: return remove_at(e.meets, e.meet_count, index);
    case EffectSection::rules: return remove_at(e.rules, e.rule_count, index);
    case EffectSection::actions:
    {
        const uint8_t r = index / max_actions_per_rule;
        if (r >= e.rule_count) return false;
        return remove_at(e.rules[r].actions, e.rules[r].action_count, index % max_actions_per_rule);
    }
    case EffectSection::starts: return remove_at(e.starts, e.start_count, index);
    default: return false;
    }
}

bool effect_duplicate(Effect &e, EffectSection section, uint8_t index)
{
    if (!effect_item_exists(e, section, index))
    {
        return false;
    }
    // Copies go at the end, so nothing that refers to items by number moves.
    switch (section)
    {
    case EffectSection::layers:
        if (e.layer_count >= max_layers_per_slot) return false;
        e.layers[e.layer_count++] = e.layers[index];
        return true;
    case EffectSection::things:
        if (e.thing_count >= max_templates_per_slot) return false;
        e.things[e.thing_count++] = e.things[index];
        return true;
    case EffectSection::sources:
        if (e.source_count >= max_emitters_per_slot) return false;
        e.sources[e.source_count++] = e.sources[index];
        return true;
    case EffectSection::meets:
        if (e.meet_count >= max_effect_meets) return false;
        e.meets[e.meet_count++] = e.meets[index];
        return true;
    case EffectSection::rules:
        if (e.rule_count >= max_effect_rules) return false;
        e.rules[e.rule_count++] = e.rules[index];
        return true;
    case EffectSection::actions:
    {
        Rule &r = e.rules[index / max_actions_per_rule];
        if (r.action_count >= max_actions_per_rule) return false;
        r.actions[r.action_count++] = r.actions[index % max_actions_per_rule];
        return true;
    }
    case EffectSection::starts:
        if (e.start_count >= max_effect_starts) return false;
        e.starts[e.start_count++] = e.starts[index];
        return true;
    default:
        return false;
    }
}

void effect_starter(Effect &e)
{
    clear_effect(e);
    std::snprintf(e.name, sizeof(e.name), "%s", "New effect");
    effect_add(e, EffectSection::layers, 0);            // a dark backdrop
    effect_add(e, EffectSection::layers, 0);
    e.layers[1].type = LayerType::entity;                // what things are drawn on
    effect_add(e, EffectSection::things, 0);             // drawn on layer 2
    e.things[0].color = {0.3f, 0.6f, 1.0f};
    effect_add(e, EffectSection::sources, 0);            // two a second from the top
    e.sources[0].color_from = ColorFrom::random_each;
    e.quota = 60;
}

// ---- running ----

void apply_effect(Engine &engine, uint8_t slot, const Effect &e)
{
    Scene &scene = engine.scene();
    Behavior &b = engine.behavior();
    int first_things = -1;
    for (uint8_t i = 0; i < e.layer_count; i++)
    {
        const Layer &src = e.layers[i];
        const LayerType type = src.type == LayerType::pixel || src.type == LayerType::empty ? LayerType::solid : src.type;
        const int l = scene.add_layer(slot, type);
        if (l < 0)
        {
            break;
        }
        Layer *dst = scene.layer(slot, static_cast<uint8_t>(l));
        const int8_t buffer = dst->buffer;
        *dst = src;
        dst->type = type;
        dst->buffer = buffer;
        if (type == LayerType::entity && first_things < 0)
        {
            first_things = l;
        }
    }
    if (first_things < 0 && e.thing_count > 0)
    {
        first_things = scene.add_layer(slot, LayerType::entity);   // somewhere to draw them
    }
    const Slot *s = scene.slot(slot);
    for (uint8_t i = 0; i < e.thing_count; i++)
    {
        Entity t = e.things[i];
        const bool drawable = t.layer < s->layer_count && s->layers[t.layer].type == LayerType::entity;
        t.layer = drawable ? t.layer : static_cast<uint8_t>(std::max(first_things, 0));
        t.owner = 0;
        b.add_template(slot, t);
    }
    for (uint8_t i = 0; i < e.source_count; i++)
    {
        Emitter m = e.sources[i];
        m.template_index = std::min<uint8_t>(m.template_index, e.thing_count ? e.thing_count - 1 : 0);
        m.attach = {};
        m.accumulator = 0.0f;
        if (m.region == Region::entity)
        {
            m.region = Region::volume;
        }
        if (e.thing_count > 0)
        {
            b.add_emitter(slot, m);
        }
    }
    for (uint8_t i = 0; i < e.meet_count; i++)
    {
        b.set_response(slot, e.meets[i].a, e.meets[i].b, e.meets[i].response);
    }
    for (uint8_t i = 0; i < e.rule_count; i++)
    {
        b.add_rule(slot, e.rules[i]);
    }
    b.set_quota(slot, e.quota);
    if (e.sets_forces)
    {
        engine.forces() = e.forces;
    }

    // What it starts with.
    const LedGeometry &g = engine.geometry();
    const World &w = engine.world();
    Rng &rng = engine.rng();
    for (uint8_t i = 0; i < e.start_count; i++)
    {
        const Start &st = e.starts[i];
        const Entity *t = b.template_at(slot, st.thing);
        if (t == nullptr)
        {
            continue;
        }
        for (uint8_t k = 0; k < st.count; k++)
        {
            Entity n = *t;
            float z = rng.range(w.floor_z, w.ceiling_z);
            if (st.place == StartPlace::top)
            {
                z = w.ceiling_z - rng.range(0.0f, 150.0f);
            }
            else if (st.place == StartPlace::bottom)
            {
                z = w.floor_z + rng.range(0.0f, 150.0f);
            }
            const float r = std::sqrt(rng.range(0.0f, 1.0f)) * g.envelope_radius(z);
            const float a = rng.range(0.0f, two_pi);
            n.pos = {r * std::cos(a), r * std::sin(a), z};
            if (st.speed > 0.0f)
            {
                const float up = rng.range(-1.0f, 1.0f);
                const float around = rng.range(0.0f, two_pi);
                const float flat = std::sqrt(std::max(0.0f, 1.0f - up * up));
                const float speed = rng.range(0.0f, st.speed);
                n.vel = {n.vel.x + speed * flat * std::cos(around), n.vel.y + speed * flat * std::sin(around),
                         n.vel.z + speed * up};
            }
            if (st.random_colors)
            {
                n.color = hsv(rng.range(0.0f, 360.0f), 1.0f, 1.0f);
            }
            b.spawn(engine, slot, n);
        }
    }
}

void capture_effect(Engine &engine, uint8_t slot, Effect &out)
{
    clear_effect(out);
    Scene &scene = engine.scene();
    Behavior &b = engine.behavior();
    const Slot *s = scene.slot(slot);
    if (s == nullptr)
    {
        return;
    }
    // Layers, less pixel ones (a Canvas's colors aren't something an effect
    // can hold): renumbered, and things follow.
    uint8_t layer_map[max_layers_per_slot];
    for (uint8_t i = 0; i < s->layer_count; i++)
    {
        layer_map[i] = 0xFF;
        const Layer &l = s->layers[i];
        if (l.type == LayerType::pixel || l.type == LayerType::empty || out.layer_count >= max_layers_per_slot)
        {
            continue;
        }
        layer_map[i] = out.layer_count;
        out.layers[out.layer_count] = l;
        out.layers[out.layer_count].buffer = -1;
        out.layer_count++;
    }
    for (uint8_t i = 0; const Entity *t = b.template_at(slot, i); i++)
    {
        Entity &c = out.things[out.thing_count++];
        c = *t;
        c.layer = t->layer < s->layer_count && layer_map[t->layer] != 0xFF ? layer_map[t->layer] : 0;
        c.owner = 0;
    }
    for (uint8_t i = 0; const Emitter *m = b.emitter(slot, i); i++)
    {
        Emitter &c = out.sources[out.source_count++];
        c = *m;
        c.attach = {};
        c.accumulator = 0.0f;
        if (c.region == Region::entity)
        {
            c.region = Region::volume;   // riding a thing isn't something an effect can say
        }
    }
    for (uint8_t i = 0; const Rule *r = b.rule(slot, i); i++)
    {
        if (out.rule_count >= max_effect_rules)
        {
            break;
        }
        out.rules[out.rule_count++] = *r;
    }
    for (uint8_t ga = 0; ga < max_groups; ga++)
    {
        for (uint8_t gb = ga; gb < max_groups; gb++)
        {
            const Response r = b.response(slot, ga, gb);
            if (r != Response::ignore && out.meet_count < max_effect_meets)
            {
                out.meets[out.meet_count++] = {ga, gb, r};
            }
        }
    }
    out.quota = b.quota(slot);
    out.sets_forces = true;
    out.forces = engine.forces();

    // What's alive now is what it starts with: counted per thing (entities
    // made without one become one), with their speed and colors.
    struct Tally
    {
        uint16_t count = 0;
        float speed_sum = 0.0f;
        Vec3 first_vel{};
        bool same_vel = true;
        Rgb first_color{};
        bool same_color = true;
        float z_min = 1e9f, z_max = -1e9f;
    };
    Tally tally[max_templates_per_slot];
    const EntityPool &pool = engine.entities();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (!pool.alive(k) || pool.item(k).slot != slot || pool.item(k).owner != 0)
        {
            continue;
        }
        const Entity &n = pool.item(k);
        int t = n.tmpl > 0 && n.tmpl - 1 < out.thing_count ? n.tmpl - 1 : -1;
        for (uint8_t i = 0; t < 0 && i < out.thing_count; i++)
        {
            const Entity &c = out.things[i];
            if (c.shape == n.shape && c.size == n.size && c.group == n.group && c.lifetime_s == n.lifetime_s)
            {
                t = i;
            }
        }
        if (t < 0)
        {
            if (out.thing_count >= max_templates_per_slot)
            {
                continue;
            }
            t = out.thing_count++;
            Entity &c = out.things[t];
            c = n;
            c.layer = n.layer < s->layer_count && layer_map[n.layer] != 0xFF ? layer_map[n.layer] : 0;
            c.vel = Vec3{};
            c.age_s = 0.0f;
        }
        Tally &ty = tally[t];
        const float speed = std::sqrt(n.vel.x * n.vel.x + n.vel.y * n.vel.y + n.vel.z * n.vel.z);
        if (ty.count == 0)
        {
            ty.first_vel = n.vel;
            ty.first_color = n.color;
        }
        else
        {
            ty.same_vel = ty.same_vel && n.vel.x == ty.first_vel.x && n.vel.y == ty.first_vel.y && n.vel.z == ty.first_vel.z;
            ty.same_color = ty.same_color && n.color.r == ty.first_color.r && n.color.g == ty.first_color.g &&
                            n.color.b == ty.first_color.b;
        }
        ty.count++;
        ty.speed_sum += speed;
        ty.z_min = std::min(ty.z_min, n.pos.z);
        ty.z_max = std::max(ty.z_max, n.pos.z);
    }
    const World &w = engine.world();
    for (uint8_t t = 0; t < out.thing_count && out.start_count < max_effect_starts; t++)
    {
        const Tally &ty = tally[t];
        if (ty.count == 0)
        {
            continue;
        }
        Start &st = out.starts[out.start_count++];
        st.thing = t;
        st.count = static_cast<uint8_t>(std::min<uint16_t>(ty.count, 100));
        if (ty.same_vel)
        {
            out.things[t].vel = ty.first_vel;   // they all move alike (a sweep's band)
            st.speed = 0.0f;
        }
        else
        {
            out.things[t].vel = Vec3{};
            st.speed = ty.speed_sum / static_cast<float>(ty.count);
        }
        if (ty.same_color)
        {
            out.things[t].color = ty.first_color;
        }
        st.random_colors = !ty.same_color;
        st.place = ty.z_min > w.ceiling_z - 200.0f ? StartPlace::top
                   : ty.z_max < w.floor_z + 200.0f  ? StartPlace::bottom
                                                     : StartPlace::anywhere;
    }
}

void effect_apply_field(Engine &engine, uint8_t slot, const Effect &e, EffectSection section, uint8_t index,
                        uint8_t field)
{
    Behavior &b = engine.behavior();
    FieldValue v;
    if (!effect_get(e, section, index, field, v))
    {
        return;
    }
    switch (section)
    {
    case EffectSection::settings:
        b.set_quota(slot, e.quota);
        if (e.sets_forces)
        {
            engine.forces() = e.forces;
        }
        break;
    case EffectSection::layers:
        if (Layer *l = engine.scene().layer(slot, index))
        {
            const int8_t buffer = l->buffer;
            const LayerType type = l->type;
            *l = e.layers[index];
            l->buffer = buffer;
            // A kind change between solid, gradient and rainbow is live; to or
            // from things needs a rebuild (the editor sends one).
            if ((type == LayerType::entity) != (l->type == LayerType::entity))
            {
                l->type = type;
            }
        }
        break;
    case EffectSection::things:
        if (Entity *t = b.template_at(slot, index))
        {
            set_thing(*t, field, v);
            // And everything already made from it (not phones' own things).
            EntityPool &pool = engine.entities_mut();
            for (uint16_t k = 0; k < pool.capacity; k++)
            {
                if (pool.alive(k) && pool.item(k).slot == slot && pool.item(k).tmpl == index + 1 &&
                    pool.item(k).owner == 0 && field != tf_vel_x && field != tf_vel_y && field != tf_vel_z)
                {
                    set_thing(pool.item(k), field, v);
                }
            }
        }
        break;
    case EffectSection::sources:
        if (Emitter *m = b.emitter(slot, index))
        {
            const float acc = m->accumulator;
            *m = e.sources[index];
            m->accumulator = acc;
            m->template_index = std::min<uint8_t>(m->template_index, e.thing_count ? e.thing_count - 1 : 0);
        }
        break;
    case EffectSection::meets:
        b.clear_responses(slot);
        for (uint8_t i = 0; i < e.meet_count; i++)
        {
            b.set_response(slot, e.meets[i].a, e.meets[i].b, e.meets[i].response);
        }
        break;
    case EffectSection::rules:
    case EffectSection::actions:
    {
        const uint8_t r = section == EffectSection::rules ? index : static_cast<uint8_t>(index / max_actions_per_rule);
        if (Rule *live = b.rule(slot, r))
        {
            // The new settings, keeping where it is in its timers and counts.
            const Rule keep = *live;
            *live = e.rules[r];
            live->fired = keep.fired;
            live->cooldown_left = keep.cooldown_left;
            live->timer_left = keep.timer_left;
            live->above = keep.above;
            b.refresh_listening(slot);
        }
        break;
    }
    default:
        break;
    }
}

// ---- JSON ----

namespace {

void json_value(Json &j, const EffectField &f, const FieldValue &v)
{
    if (f.type == FieldType::color)
    {
        j.hex(v.c);
    }
    else
    {
        j.num(v.f);
    }
}

void json_item(Json &j, const Effect &e, EffectSection section, uint8_t index)
{
    const EffectSectionInfo &info = effect_section(section);
    for (uint8_t f = 0; f < info.field_count; f++)
    {
        FieldValue v;
        effect_get(e, section, index, f, v);
        j.raw(f ? "," : "");
        json_value(j, info.fields[f], v);
    }
}

}  // namespace

size_t effect_section_json(const Effect &e, EffectSection section, char *out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    Json j{out, cap};
    j.raw("{\"s\":%u,\"n\":", static_cast<unsigned>(section));
    j.str(e.name);
    j.raw(",\"i\":[");
    bool first = true;
    if (section == EffectSection::actions)
    {
        for (uint8_t r = 0; r < e.rule_count; r++)
        {
            for (uint8_t a = 0; a < e.rules[r].action_count; a++)
            {
                j.raw("%s[%u,%u,", first ? "" : ",", r, a);
                json_item(j, e, section, static_cast<uint8_t>(r * max_actions_per_rule + a));
                j.raw("]");
                first = false;
            }
        }
    }
    else
    {
        for (uint8_t i = 0; i < effect_item_count(e, section); i++)
        {
            j.raw("%s[", first ? "" : ",");
            json_item(j, e, section, i);
            j.raw("]");
            first = false;
        }
    }
    j.raw("]}");
    return j.len;
}

size_t effect_schema_json(EffectSection section, char *out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    const EffectSectionInfo &info = effect_section(section);
    Json j{out, cap};
    j.raw("{\"s\":%u,\"id\":\"%s\",\"l\":\"%s\",\"item\":\"%s\",\"max\":%u,\"f\":[", static_cast<unsigned>(section),
          info.id, info.label, info.item, static_cast<unsigned>(info.max_items));
    for (uint8_t i = 0; i < info.field_count; i++)
    {
        const EffectField &f = info.fields[i];
        static const char *const types[] = {"n", "c", "ch", "t"};
        j.raw("%s{\"id\":\"%s\",\"l\":", i ? "," : "", f.id);
        j.str(f.label);
        j.raw(",\"t\":\"%s\"", types[static_cast<int>(f.type)]);
        if (f.type == FieldType::number)
        {
            if (f.min != 0.0f)
            {
                j.raw(",\"min\":");
                j.num(f.min);
            }
            if (f.max != 1.0f)
            {
                j.raw(",\"max\":");
                j.num(f.max);
            }
            if (f.step != 0.0f)
            {
                j.raw(",\"st\":");
                j.num(f.step);
            }
        }
        if (f.choices != nullptr)
        {
            j.raw(",\"ch\":");
            j.str(f.choices);
        }
        if (f.shown_by >= 0)
        {
            j.raw(",\"by\":%d,\"when\":%u", f.shown_by, static_cast<unsigned>(f.shown_when));
        }
        j.raw("}");
    }
    j.raw("]}");
    return j.len;
}

// ---- stored form ----
//
// Version 1, little-endian: u8 version, u8 name length, name, then per
// section (settings .. starts, in order): u8 section, u8 items, and per item
// [u8 rule, for actions] u8 fields, then (u8 field, u8 type, value) x fields:
// type 0 an f32, 1 a color's r g b bytes. An item holds only the fields that
// are shown (what's hidden is unused, and aliases - a wedge's width is its
// size - stay single) and differ from a new item's; settings hold every
// shown field.

namespace {

constexpr uint8_t effect_format = 1;

struct Out
{
    uint8_t *p;
    size_t cap;
    size_t len = 0;
    bool ok = true;
    void byte(uint8_t v)
    {
        if (len < cap) p[len++] = v; else ok = false;
    }
    void f32(float v)
    {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        for (int k = 0; k < 4; k++) byte(static_cast<uint8_t>(bits >> (8 * k)));
    }
};

struct In
{
    const uint8_t *p;
    size_t len;
    size_t pos = 0;
    bool ok = true;
    uint8_t byte()
    {
        if (pos < len) return p[pos++];
        ok = false;
        return 0;
    }
    float f32()
    {
        uint32_t bits = 0;
        for (int k = 0; k < 4; k++) bits |= static_cast<uint32_t>(byte()) << (8 * k);
        float v;
        std::memcpy(&v, &bits, 4);
        if (!std::isfinite(v))
        {
            ok = false;
            return 0.0f;
        }
        return v;
    }
};

// A new item of each section, to compare against.
struct Defaults
{
    Layer layer;
    Entity thing;
    Emitter source;
    Meet meet;
    Rule rule;
    Action action;
    Start start;
};

Defaults make_defaults()
{
    Defaults d;
    new_layer(d.layer);
    new_thing(d.thing);
    new_source(d.source);
    new_rule(d.rule);
    new_action(d.action);
    new_start(d.start);
    return d;
}

const Defaults defaults = make_defaults();

bool default_get(EffectSection section, uint8_t f, FieldValue &v)
{
    switch (section)
    {
    case EffectSection::layers: return get_layer(defaults.layer, f, v);
    case EffectSection::things: return get_thing(defaults.thing, f, v);
    case EffectSection::sources: return get_source(defaults.source, f, v);
    case EffectSection::meets: return get_meet(defaults.meet, f, v);
    case EffectSection::rules: return get_rule(defaults.rule, f, v);
    case EffectSection::actions: return get_action(defaults.action, f, v);
    case EffectSection::starts: return get_start(defaults.start, f, v);
    default: return false;   // settings: always stored
    }
}

bool same(const EffectField &f, const FieldValue &a, const FieldValue &b)
{
    if (f.type == FieldType::color)
    {
        return unit_to_byte(a.c.r) == unit_to_byte(b.c.r) && unit_to_byte(a.c.g) == unit_to_byte(b.c.g) &&
               unit_to_byte(a.c.b) == unit_to_byte(b.c.b);
    }
    return a.f == b.f;
}

// Whether field f of the item is to be stored.
bool worth_storing(const Effect &e, EffectSection section, uint8_t index, uint8_t f, FieldValue &v)
{
    const EffectSectionInfo &info = effect_section(section);
    const EffectField &field = info.fields[f];
    if (!effect_get(e, section, index, f, v) || !effect_field_shown(e, section, index, f))
    {
        return false;
    }
    FieldValue d;
    return !default_get(section, f, d) || !same(field, v, d);
}

void store_item(Out &o, const Effect &e, EffectSection section, uint8_t index)
{
    const EffectSectionInfo &info = effect_section(section);
    uint8_t n = 0;
    FieldValue v;
    for (uint8_t f = 0; f < info.field_count; f++)
    {
        n += worth_storing(e, section, index, f, v);
    }
    o.byte(n);
    for (uint8_t f = 0; f < info.field_count; f++)
    {
        if (!worth_storing(e, section, index, f, v))
        {
            continue;
        }
        o.byte(f);
        if (info.fields[f].type == FieldType::color)
        {
            o.byte(1);
            o.byte(unit_to_byte(v.c.r));
            o.byte(unit_to_byte(v.c.g));
            o.byte(unit_to_byte(v.c.b));
        }
        else
        {
            o.byte(0);
            o.f32(v.f);
        }
    }
}

Effect scratch_effect;

}  // namespace

Effect &effect_scratch() { return scratch_effect; }

bool effect_field_shown(const Effect &e, EffectSection section, uint8_t index, uint8_t field)
{
    const EffectSectionInfo &info = effect_section(section);
    if (field >= info.field_count)
    {
        return false;
    }
    const EffectField &f = info.fields[field];
    if (f.shown_by < 0)
    {
        return true;
    }
    FieldValue by;
    if (!effect_get(e, section, index, static_cast<uint8_t>(f.shown_by), by))
    {
        return false;
    }
    const int k = as_int(by.f);
    return k >= 0 && k < 16 && ((f.shown_when >> k) & 1u) != 0 &&
           effect_field_shown(e, section, index, static_cast<uint8_t>(f.shown_by));
}

size_t effect_save(const Effect &e, uint8_t *data, size_t cap)
{
    Out o{data, cap};
    o.byte(effect_format);
    const size_t n = strnlen(e.name, name_size - 1);
    o.byte(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; i++) o.byte(static_cast<uint8_t>(e.name[i]));
    for (uint8_t s = 0; s < static_cast<uint8_t>(EffectSection::count); s++)
    {
        const EffectSection section = static_cast<EffectSection>(s);
        o.byte(s);
        o.byte(effect_item_count(e, section));
        if (section == EffectSection::actions)
        {
            for (uint8_t r = 0; r < e.rule_count; r++)
            {
                for (uint8_t a = 0; a < e.rules[r].action_count; a++)
                {
                    o.byte(r);
                    store_item(o, e, section, static_cast<uint8_t>(r * max_actions_per_rule + a));
                }
            }
        }
        else
        {
            for (uint8_t i = 0; i < effect_item_count(e, section); i++)
            {
                store_item(o, e, section, i);
            }
        }
    }
    return o.ok ? o.len : 0;
}

bool effect_load(Effect &e, const uint8_t *data, size_t len)
{
    clear_effect(e);
    In in{data, len};
    if (in.byte() != effect_format)
    {
        return false;
    }
    const uint8_t n = in.byte();
    if (n >= name_size)
    {
        return false;
    }
    for (uint8_t i = 0; i < n; i++) e.name[i] = static_cast<char>(in.byte());
    e.name[n] = '\0';
    for (uint8_t s = 0; s < static_cast<uint8_t>(EffectSection::count) && in.ok; s++)
    {
        if (in.byte() != s)
        {
            return false;
        }
        const EffectSection section = static_cast<EffectSection>(s);
        const EffectSectionInfo &info = effect_section(section);
        const uint8_t items = in.byte();
        if (section == EffectSection::settings && items != 1)
        {
            return false;
        }
        for (uint8_t k = 0; k < items && in.ok; k++)
        {
            uint8_t index = 0;
            if (section == EffectSection::actions)
            {
                const uint8_t r = in.byte();
                if (r >= e.rule_count || !add_item(e, section, r))
                {
                    return false;
                }
                index = static_cast<uint8_t>(r * max_actions_per_rule + e.rules[r].action_count - 1);
            }
            else if (section != EffectSection::settings)
            {
                if (!add_item(e, section, 0))
                {
                    return false;
                }
                index = static_cast<uint8_t>(effect_item_count(e, section) - 1);
            }
            const uint8_t fields = in.byte();
            for (uint8_t f = 0; f < fields && in.ok; f++)
            {
                const uint8_t field = in.byte();
                const uint8_t type = in.byte();
                FieldValue v;
                if (type == 1)
                {
                    const uint8_t r = in.byte();
                    const uint8_t g = in.byte();
                    const uint8_t b = in.byte();
                    v.c = to_rgb(r, g, b);
                }
                else if (type == 0)
                {
                    v.f = in.f32();
                }
                else
                {
                    return false;
                }
                // Fields this firmware doesn't know (from a later one) are skipped.
                if (field < info.field_count)
                {
                    effect_set(e, section, index, field, v);
                }
            }
        }
    }
    return in.ok && in.pos == len;
}

}  // namespace neotree
