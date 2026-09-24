#pragma once
// Fixed-capacity object pool with generation-checked handles. Entities (and
// later emitters, rules' runtime state, etc.) live in these: no heap, and a
// handle to something that has since been freed - even if its slot has been
// reused - reliably resolves to nullptr instead of the new occupant.

#include <cstdint>

namespace neotree {

struct Handle
{
    uint16_t index = 0;
    uint16_t generation = 0;   // 0 is never issued, so a default Handle is invalid

    bool valid() const { return generation != 0; }
    bool operator==(const Handle &) const = default;
};

template <typename T, uint16_t Capacity>
class Pool
{
public:
    static constexpr uint16_t capacity = Capacity;

    Pool() { clear(); }

    // Frees everything. Outstanding handles all become stale.
    void clear()
    {
        count_ = 0;
        free_head_ = 0;
        for (uint16_t i = 0; i < Capacity; i++)
        {
            alive_[i] = false;
            next_free_[i] = static_cast<uint16_t>(i + 1);
            generation_[i] = bump(generation_[i]);
        }
    }

    // Returns an invalid handle when full. The object is value-initialized.
    Handle create()
    {
        if (free_head_ >= Capacity)
        {
            return {};
        }
        uint16_t i = free_head_;
        free_head_ = next_free_[i];
        alive_[i] = true;
        items_[i] = T{};
        count_++;
        return {i, generation_[i]};
    }

    bool destroy(Handle h)
    {
        if (!owns(h))
        {
            return false;
        }
        alive_[h.index] = false;
        generation_[h.index] = bump(generation_[h.index]);
        next_free_[h.index] = free_head_;
        free_head_ = h.index;
        count_--;
        return true;
    }

    T *get(Handle h) { return owns(h) ? &items_[h.index] : nullptr; }
    const T *get(Handle h) const { return owns(h) ? &items_[h.index] : nullptr; }

    uint16_t size() const { return count_; }
    bool full() const { return count_ == Capacity; }

    // Iteration by slot index: for (i < capacity) if (alive(i)) ... item(i).
    bool alive(uint16_t index) const { return index < Capacity && alive_[index]; }
    T &item(uint16_t index) { return items_[index]; }
    const T &item(uint16_t index) const { return items_[index]; }
    Handle handle_at(uint16_t index) const { return alive(index) ? Handle{index, generation_[index]} : Handle{}; }

private:
    bool owns(Handle h) const
    {
        return h.valid() && h.index < Capacity && alive_[h.index] && generation_[h.index] == h.generation;
    }
    static uint16_t bump(uint16_t g) { return static_cast<uint16_t>(g == 0xFFFF ? 1 : g + 1); }

    T items_[Capacity] = {};
    uint16_t generation_[Capacity] = {};
    uint16_t next_free_[Capacity] = {};
    bool alive_[Capacity] = {};
    uint16_t free_head_ = 0;
    uint16_t count_ = 0;
};

}  // namespace neotree
