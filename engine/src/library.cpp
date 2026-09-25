#include "neotree/library.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "json.hpp"
#include "neotree/scene.hpp"

namespace neotree {

namespace {

using detail::Json;

// ---- built-in presets ----

constexpr uint8_t count_of_presets = 5;
SceneSpec presets[count_of_presets];
constexpr uint8_t count_of_shows = 1;
Show shows[count_of_shows];
bool built = false;

void name_scene(SceneSpec &scene, const char *name)
{
    std::snprintf(scene.name, sizeof(scene.name), "%s", name);
}

void add_entry(Show &show, const char *preset, uint16_t seconds)
{
    ShowEntry &e = show.entries[show.count++];
    std::snprintf(e.preset, sizeof(e.preset), "%s", preset);
    e.duration_s = seconds;
}

void build()
{
    if (built)
    {
        return;
    }
    built = true;

    // 0: today's manual colors - the default base scene.
    SceneSpec &colors = presets[0];
    name_scene(colors, "Colors");
    colors.specs[0] = SlotSpec::of("canvas");

    // 1: two modes stacked - snow (no backdrop) over a slow rainbow.
    SceneSpec &snowbow = presets[1];
    name_scene(snowbow, "Snow on rainbow");
    snowbow.specs[0] = SlotSpec::of("rainbow").set("speed", 0.08f).set("brightness", 0.45f);
    snowbow.specs[1] = SlotSpec::of("snow").set("backdrop", 0.0f);

    // 2: fireworks for 30 s, then snow for 45 s, then fireworks again,
    // forever, over a night-sky gradient.
    SceneSpec &show = presets[2];
    name_scene(show, "Holiday show");
    show.specs[0] = SlotSpec::of("gradient").set("bottom", Rgb{0.0f, 0.02f, 0.12f}).set("top", Rgb{0.08f, 0.0f, 0.1f});
    show.specs[1] = SlotSpec::of("fireworks").set("backdrop", 0.0f);
    show.specs[1].life.duration_s = 30.0f;
    show.specs[1].life.policy = EndPolicy::chain;
    show.specs[1].life.next = 4;
    show.specs[4] = SlotSpec::of("snow").set("backdrop", 0.0f);
    show.specs[4].life.duration_s = 45.0f;
    show.specs[4].life.policy = EndPolicy::chain;
    show.specs[4].life.next = 1;
    show.specs[4].life.drain_timeout_s = 14.0f;   // flakes live 12 s: let them all settle and fade

    // 3: the Pi's three sweeps in turn, by cycles: 5 linear passes, 3
    // drops, 3 launches, repeat.
    SceneSpec &sweeps = presets[3];
    name_scene(sweeps, "Sweep tour");
    sweeps.specs[0] = SlotSpec::of("solid").set("color", to_rgb(0, 140, 0));
    sweeps.specs[1] = SlotSpec::of("sweep").set("motion", 0.0f).set("backdrop", 0.0f);
    sweeps.specs[1].life.cycles = 5;
    sweeps.specs[1].life.policy = EndPolicy::chain;
    sweeps.specs[1].life.next = 4;
    sweeps.specs[4] = SlotSpec::of("sweep").set("motion", 1.0f).set("backdrop", 0.0f);
    sweeps.specs[4].life.cycles = 3;
    sweeps.specs[4].life.policy = EndPolicy::chain;
    sweeps.specs[4].life.next = 5;
    sweeps.specs[5] = SlotSpec::of("sweep").set("motion", 2.0f).set("backdrop", 0.0f);
    sweeps.specs[5].life.cycles = 3;
    sweeps.specs[5].life.policy = EndPolicy::chain;
    sweeps.specs[5].life.next = 1;

    // 4: a dozen fireworks over the family's colors, then back to just the
    // colors (revert: the base scene has nothing in slot 1).
    SceneSpec &finale = presets[4];
    name_scene(finale, "Fireworks finale");
    finale.specs[0] = SlotSpec::of("canvas");
    finale.specs[1] = SlotSpec::of("fireworks").set("backdrop", 0.0f).set("rate", 1.5f);
    finale.specs[1].life.cycles = 12;
    finale.specs[1].life.policy = EndPolicy::revert;

    // A show to leave running for an evening: ~22 minutes a round, forever.
    Show &evening = shows[0];
    std::snprintf(evening.name, sizeof(evening.name), "%s", "Holiday evening");
    add_entry(evening, "Snow on rainbow", 240);
    add_entry(evening, "Holiday show", 600);
    add_entry(evening, "Sweep tour", 180);
    add_entry(evening, "Fireworks finale", 90);
    add_entry(evening, "Colors", 210);
}

// ---- stored form ----
//
// Version 1, little-endian:
//   u8 version, u8 flags (bit 0: custom base scene), str boot show,
//   [scene: the base scene, if custom], u8 n, scene x n (user presets),
//   u8 n, show x n (user shows)
//   scene: str name, f32 duration, u8 policy, u8 n, spec x n
//   spec: u8 index, str mode id, f32 opacity, lifecycle, u8 n,
//         (str param id, f32 value, u8 r, u8 g, u8 b) x n
//   lifecycle: f32 duration, u16 cycles, u8 drain, f32 drain timeout,
//              u8 policy, u16 repeats, u8 next, u8 outcome, u8 outcome
//              policy, u8 outcome next, u8 transition, f32 transition time
//   show: str name, u8 flags (bit 0 loop, bit 1 shuffle), u8 n,
//         (str preset, u16 seconds) x n
//   str: u8 length (< name_size), then the bytes
constexpr uint8_t format_version = 1;
constexpr uint8_t max_stored_params = 16;   // more than any mode has: room for later ones

struct Writer
{
    uint8_t *out;
    size_t cap;
    size_t len = 0;
    bool ok = true;

    void bytes(const void *p, size_t n)
    {
        if (len + n > cap)
        {
            ok = false;
            return;
        }
        std::memcpy(out + len, p, n);
        len += n;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v)
    {
        const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        bytes(b, 2);
    }
    void f32(float v)
    {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        const uint8_t b[4] = {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
                              static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24)};
        bytes(b, 4);
    }
    void str(const char *s)
    {
        const size_t n = strnlen(s, name_size - 1);
        u8(static_cast<uint8_t>(n));
        bytes(s, n);
    }
};

struct Reader
{
    const uint8_t *data;
    size_t len;
    size_t pos = 0;
    bool ok = true;

    const uint8_t *take(size_t n)
    {
        if (!ok || pos + n > len)
        {
            ok = false;
            return nullptr;
        }
        const uint8_t *p = data + pos;
        pos += n;
        return p;
    }
    uint8_t u8()
    {
        const uint8_t *p = take(1);
        return p ? p[0] : 0;
    }
    uint16_t u16()
    {
        const uint8_t *p = take(2);
        return p ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : 0;
    }
    float f32()
    {
        const uint8_t *p = take(4);
        if (p == nullptr)
        {
            return 0.0f;
        }
        const uint32_t bits = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
        float v;
        std::memcpy(&v, &bits, 4);
        if (!std::isfinite(v))
        {
            ok = false;
            return 0.0f;
        }
        return v;
    }
    void str(char (&dst)[name_size])
    {
        const uint8_t n = u8();
        if (n >= name_size)
        {
            ok = false;
        }
        const uint8_t *p = take(n);
        if (p == nullptr)
        {
            dst[0] = '\0';
            return;
        }
        std::memcpy(dst, p, n);
        dst[n] = '\0';
    }
    // Enum and index checks: anything out of range makes the data invalid.
    void check(bool condition) { ok = ok && condition; }
};

bool policy_ok(uint8_t p) { return p <= static_cast<uint8_t>(EndPolicy::hold); }
bool spec_ref_ok(uint8_t i) { return i < max_specs || i == no_spec; }

void write_life(Writer &w, const Lifecycle &l)
{
    w.f32(l.duration_s);
    w.u16(l.cycles);
    w.u8(l.drain ? 1 : 0);
    w.f32(l.drain_timeout_s);
    w.u8(static_cast<uint8_t>(l.policy));
    w.u16(l.repeats);
    w.u8(l.next);
    w.u8(l.outcome);
    w.u8(static_cast<uint8_t>(l.outcome_policy));
    w.u8(l.outcome_next);
    w.u8(static_cast<uint8_t>(l.transition));
    w.f32(l.transition_s);
}

void read_life(Reader &r, Lifecycle &l)
{
    l.duration_s = r.f32();
    l.cycles = r.u16();
    l.drain = r.u8() != 0;
    l.drain_timeout_s = r.f32();
    const uint8_t policy = r.u8();
    l.repeats = r.u16();
    l.next = r.u8();
    l.outcome = r.u8();
    const uint8_t outcome_policy = r.u8();
    l.outcome_next = r.u8();
    const uint8_t transition = r.u8();
    l.transition_s = r.f32();
    r.check(policy_ok(policy) && policy_ok(outcome_policy) && transition <= static_cast<uint8_t>(Transition::fade) &&
            spec_ref_ok(l.next) && spec_ref_ok(l.outcome_next) && l.duration_s >= 0.0f &&
            l.drain_timeout_s >= 0.0f && l.transition_s >= 0.0f);
    l.policy = static_cast<EndPolicy>(policy_ok(policy) ? policy : 0);
    l.outcome_policy = static_cast<EndPolicy>(policy_ok(outcome_policy) ? outcome_policy : 0);
    l.transition = transition ? Transition::fade : Transition::cut;
}

void write_scene(Writer &w, const SceneSpec &scene)
{
    w.str(scene.name);
    w.f32(scene.duration_s);
    w.u8(static_cast<uint8_t>(scene.policy));
    uint8_t n = 0;
    for (const SlotSpec &s : scene.specs)
    {
        n += mode_at(s.mode) != nullptr;
    }
    w.u8(n);
    for (uint8_t i = 0; i < max_specs; i++)
    {
        const SlotSpec &s = scene.specs[i];
        const ModeDef *def = mode_at(s.mode);
        if (def == nullptr)
        {
            continue;
        }
        w.u8(i);
        w.str(def->id);
        w.f32(s.opacity);
        write_life(w, s.life);
        w.u8(def->param_count);
        for (uint8_t p = 0; p < def->param_count; p++)
        {
            w.str(def->params[p].id);
            w.f32(s.params[p].f);
            w.u8(unit_to_byte(s.params[p].c.r));
            w.u8(unit_to_byte(s.params[p].c.g));
            w.u8(unit_to_byte(s.params[p].c.b));
        }
    }
}

void read_scene(Reader &r, SceneSpec &scene)
{
    clear_scene(scene);
    r.str(scene.name);
    scene.duration_s = r.f32();
    const uint8_t policy = r.u8();
    r.check(policy_ok(policy) && scene.duration_s >= 0.0f);
    scene.policy = static_cast<EndPolicy>(policy_ok(policy) ? policy : 0);
    const uint8_t n = r.u8();
    r.check(n <= max_specs);
    for (uint8_t k = 0; k < n && r.ok; k++)
    {
        const uint8_t index = r.u8();
        r.check(index < max_specs);
        char id[name_size];
        r.str(id);
        if (!r.ok)
        {
            return;
        }
        // Unknown mode (removed from the firmware since): the slot stays
        // empty, but the rest of the entry must still be read past.
        SlotSpec &spec = scene.specs[index];
        spec = SlotSpec::of(id);
        spec.opacity = r.f32();
        read_life(r, spec.life);
        const uint8_t params = r.u8();
        r.check(params <= max_stored_params);
        const ModeDef *def = mode_at(spec.mode);
        for (uint8_t p = 0; p < params && r.ok; p++)
        {
            char param_id[name_size];
            r.str(param_id);
            ParamValue v;
            v.f = r.f32();
            const uint8_t cr = r.u8();
            const uint8_t cg = r.u8();
            const uint8_t cb = r.u8();
            v.c = to_rgb(cr, cg, cb);
            for (uint8_t i = 0; def != nullptr && i < def->param_count; i++)
            {
                if (std::strcmp(def->params[i].id, param_id) == 0)
                {
                    spec.params[i] = v;
                }
            }
        }
        if (def == nullptr)
        {
            spec = SlotSpec{};
        }
    }
}

void write_show(Writer &w, const Show &show)
{
    w.str(show.name);
    w.u8(static_cast<uint8_t>((show.loop ? 1 : 0) | (show.shuffle ? 2 : 0)));
    w.u8(show.count);
    for (uint8_t i = 0; i < show.count; i++)
    {
        w.str(show.entries[i].preset);
        w.u16(show.entries[i].duration_s);
    }
}

void read_show(Reader &r, Show &show)
{
    clear_show(show);
    r.str(show.name);
    const uint8_t flags = r.u8();
    show.loop = (flags & 1) != 0;
    show.shuffle = (flags & 2) != 0;
    show.count = r.u8();
    r.check(show.count <= max_show_entries);
    for (uint8_t i = 0; i < show.count && r.ok; i++)
    {
        r.str(show.entries[i].preset);
        show.entries[i].duration_s = r.u16();
    }
}

}  // namespace

// ---- built-ins ----

uint8_t preset_count()
{
    build();
    return count_of_presets;
}

const SceneSpec &preset_at(uint8_t index)
{
    build();
    return presets[index < count_of_presets ? index : 0];
}

uint8_t find_preset(const char *name)
{
    build();
    for (uint8_t i = 0; i < count_of_presets; i++)
    {
        if (std::strcmp(presets[i].name, name) == 0)
        {
            return i;
        }
    }
    return no_index;
}

uint8_t builtin_show_count()
{
    build();
    return count_of_shows;
}

const Show &builtin_show_at(uint8_t index)
{
    build();
    return shows[index < count_of_shows ? index : 0];
}

void clear_scene(SceneSpec &scene)
{
    scene.name[0] = '\0';
    for (SlotSpec &spec : scene.specs)
    {
        spec = SlotSpec{};
    }
    scene.duration_s = 0.0f;
    scene.policy = EndPolicy::hold;
}

void clear_show(Show &show)
{
    show.name[0] = '\0';
    for (ShowEntry &e : show.entries)
    {
        e.preset[0] = '\0';
        e.duration_s = 0;
    }
    show.count = 0;
    show.loop = true;
    show.shuffle = false;
}

bool copy_name(char (&dst)[name_size], const char *src, size_t src_len)
{
    size_t n = 0;
    for (size_t i = 0; i < src_len && src[i] != '\0' && n + 1 < name_size; i++)
    {
        const unsigned char ch = static_cast<unsigned char>(src[i]);
        if (ch >= 0x20 && ch != 0x7F && !(n == 0 && ch == ' '))
        {
            dst[n++] = static_cast<char>(ch);
        }
    }
    while (n > 0 && dst[n - 1] == ' ')
    {
        n--;
    }
    dst[n] = '\0';
    return n > 0;
}

// ---- Library ----

void Library::reset()
{
    build();
    user_preset_count_ = 0;
    user_show_count_ = 0;
    base_ = presets[0];
    base_custom_ = false;
    boot_show_[0] = '\0';
    revision_++;
}

uint8_t Library::preset_count() const { return static_cast<uint8_t>(count_of_presets + user_preset_count_); }

const SceneSpec &Library::preset(uint8_t index) const
{
    if (index < count_of_presets)
    {
        return preset_at(index);
    }
    index = static_cast<uint8_t>(index - count_of_presets);
    return index < user_preset_count_ ? user_presets_[index] : preset_at(0);
}

bool Library::preset_is_user(uint8_t index) const
{
    return index >= count_of_presets && index < preset_count();
}

uint8_t Library::find_preset(const char *name) const
{
    for (uint8_t i = 0; i < preset_count(); i++)
    {
        if (std::strcmp(preset(i).name, name) == 0)
        {
            return i;
        }
    }
    return no_index;
}

uint8_t Library::save_preset(const SceneSpec &scene, const char *name)
{
    char clean[name_size];
    if (!copy_name(clean, name, name_size))
    {
        return no_index;
    }
    uint8_t index = find_preset(clean);
    if (index != no_index && !preset_is_user(index))
    {
        return no_index;   // can't overwrite a built-in
    }
    if (index == no_index)
    {
        if (user_preset_count_ >= max_user_presets)
        {
            return no_index;
        }
        index = static_cast<uint8_t>(count_of_presets + user_preset_count_++);
    }
    SceneSpec &dst = user_presets_[index - count_of_presets];
    dst = scene;
    std::memcpy(dst.name, clean, name_size);
    revision_++;
    return index;
}

bool Library::delete_preset(uint8_t index)
{
    if (!preset_is_user(index))
    {
        return false;
    }
    for (uint8_t i = static_cast<uint8_t>(index - count_of_presets); i + 1 < user_preset_count_; i++)
    {
        user_presets_[i] = user_presets_[i + 1];
    }
    user_preset_count_--;
    revision_++;
    return true;
}

uint8_t Library::show_count() const { return static_cast<uint8_t>(count_of_shows + user_show_count_); }

const Show &Library::show(uint8_t index) const
{
    if (index < count_of_shows)
    {
        return builtin_show_at(index);
    }
    index = static_cast<uint8_t>(index - count_of_shows);
    return index < user_show_count_ ? user_shows_[index] : builtin_show_at(0);
}

bool Library::show_is_user(uint8_t index) const
{
    return index >= count_of_shows && index < show_count();
}

uint8_t Library::find_show(const char *name) const
{
    for (uint8_t i = 0; name[0] != '\0' && i < show_count(); i++)
    {
        if (std::strcmp(show(i).name, name) == 0)
        {
            return i;
        }
    }
    return no_index;
}

uint8_t Library::save_show(const Show &s)
{
    char clean[name_size];
    if (!copy_name(clean, s.name, name_size) || s.count > max_show_entries)
    {
        return no_index;
    }
    uint8_t index = find_show(clean);
    if (index != no_index && !show_is_user(index))
    {
        return no_index;
    }
    if (index == no_index)
    {
        if (user_show_count_ >= max_user_shows)
        {
            return no_index;
        }
        index = static_cast<uint8_t>(count_of_shows + user_show_count_++);
    }
    Show &dst = user_shows_[index - count_of_shows];
    dst = s;
    std::memcpy(dst.name, clean, name_size);
    revision_++;
    return index;
}

bool Library::delete_show(uint8_t index)
{
    if (!show_is_user(index))
    {
        return false;
    }
    if (std::strcmp(show(index).name, boot_show_) == 0)
    {
        boot_show_[0] = '\0';
    }
    for (uint8_t i = static_cast<uint8_t>(index - count_of_shows); i + 1 < user_show_count_; i++)
    {
        user_shows_[i] = user_shows_[i + 1];
    }
    user_show_count_--;
    revision_++;
    return true;
}

void Library::set_base(const SceneSpec &scene)
{
    base_ = scene;
    base_custom_ = true;
    revision_++;
}

void Library::reset_base()
{
    base_ = preset_at(0);
    base_custom_ = false;
    revision_++;
}

bool Library::set_boot_show(uint8_t index)
{
    if (index == no_index)
    {
        boot_show_[0] = '\0';
    }
    else if (index < show_count())
    {
        std::memcpy(boot_show_, show(index).name, name_size);
    }
    else
    {
        return false;
    }
    revision_++;
    return true;
}

size_t Library::save(uint8_t *out, size_t cap) const
{
    Writer w{out, cap};
    w.u8(format_version);
    w.u8(base_custom_ ? 1 : 0);
    w.str(boot_show_);
    if (base_custom_)
    {
        write_scene(w, base_);
    }
    w.u8(user_preset_count_);
    for (uint8_t i = 0; i < user_preset_count_; i++)
    {
        write_scene(w, user_presets_[i]);
    }
    w.u8(user_show_count_);
    for (uint8_t i = 0; i < user_show_count_; i++)
    {
        write_show(w, user_shows_[i]);
    }
    return w.ok ? w.len : 0;
}

bool Library::load(const uint8_t *data, size_t len)
{
    reset();
    Reader r{data, len};
    r.check(r.u8() == format_version);
    const uint8_t flags = r.u8();
    r.str(boot_show_);
    if (r.ok && (flags & 1) != 0)
    {
        read_scene(r, base_);
        base_custom_ = true;
    }
    const uint8_t presets_n = r.u8();
    r.check(presets_n <= max_user_presets);
    for (uint8_t i = 0; i < presets_n && r.ok; i++)
    {
        read_scene(r, user_presets_[i]);
    }
    user_preset_count_ = r.ok ? presets_n : 0;
    const uint8_t shows_n = r.u8();
    r.check(shows_n <= max_user_shows);
    for (uint8_t i = 0; i < shows_n && r.ok; i++)
    {
        read_show(r, user_shows_[i]);
    }
    user_show_count_ = r.ok ? shows_n : 0;
    r.check(r.pos == len);
    if (!r.ok)
    {
        reset();
        return false;
    }
    revision_++;
    return true;
}

size_t Library::describe(char *out, size_t cap) const
{
    if (cap == 0)
    {
        return 0;
    }
    Json j{out, cap};
    j.raw("{\"rev\":%lu,\"presets\":[", static_cast<unsigned long>(revision_));
    for (uint8_t i = 0; i < preset_count(); i++)
    {
        j.raw("%s{\"n\":", i ? "," : "");
        j.str(preset(i).name);
        j.raw("%s}", preset_is_user(i) ? ",\"u\":1" : "");
    }
    j.raw("],\"shows\":[");
    for (uint8_t i = 0; i < show_count(); i++)
    {
        const Show &s = show(i);
        j.raw("%s{\"n\":", i ? "," : "");
        j.str(s.name);
        j.raw("%s,\"loop\":%d,\"shuffle\":%d,\"e\":[", show_is_user(i) ? ",\"u\":1" : "", s.loop ? 1 : 0,
              s.shuffle ? 1 : 0);
        for (uint8_t k = 0; k < s.count; k++)
        {
            const uint8_t p = find_preset(s.entries[k].preset);
            j.raw("%s[%d,%u]", k ? "," : "", p == no_index ? -1 : static_cast<int>(p),
                  static_cast<unsigned>(s.entries[k].duration_s));
        }
        j.raw("]}");
    }
    j.raw("],\"boot\":");
    j.str(boot_show_);
    j.raw(",\"base\":{\"custom\":%d,\"slots\":[", base_custom_ ? 1 : 0);
    for (uint8_t s = 0; s < max_slots; s++)
    {
        const ModeDef *def = mode_at(base_.specs[s].mode);
        j.raw("%s\"%s\"", s ? "," : "", def != nullptr ? def->id : "");
    }
    j.raw("]}}");
    return j.len;
}

}  // namespace neotree
