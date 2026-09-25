#pragma once

#include <cstdint>
#include <vector>

// Open-addressing hash map from uint64 keys to uint32 values.
// std::unordered_map allocates a node on the heap for every insert; this
// keeps everything in one flat array, so an insert is usually just a store.
// Linear probing, and "backward shift" on erase so no tombstones pile up.
// The key UINT64_MAX is reserved to mean "empty slot".
class FlatHashMap {
public:
    static constexpr uint64_t kEmpty = UINT64_MAX;

    explicit FlatHashMap(size_t expected = 1024) {
        size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;  // keep load factor <= 0.5
        slots_.assign(cap, Slot{kEmpty, 0});
        mask_ = cap - 1;
    }

    // returns false if the key was already there
    bool insert(uint64_t key, uint32_t value) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        size_t i = hash(key) & mask_;
        while (slots_[i].key != kEmpty) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i] = {key, value};
        ++size_;
        return true;
    }

    // returns pointer to the value, or nullptr if missing
    const uint32_t* find(uint64_t key) const {
        size_t i = hash(key) & mask_;
        while (slots_[i].key != kEmpty) {
            if (slots_[i].key == key) return &slots_[i].value;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    bool contains(uint64_t key) const { return find(key) != nullptr; }

    bool erase(uint64_t key) {
        size_t i = hash(key) & mask_;
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmpty) return false;
            i = (i + 1) & mask_;
        }
        // Shift later entries of the same probe chain back into the hole,
        // otherwise a lookup would stop early at the empty slot.
        size_t hole = i;
        size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmpty) break;
            size_t home = hash(slots_[j].key) & mask_;
            // move j into the hole if its home is not in (hole, j] (cyclically)
            bool between = hole <= j ? (hole < home && home <= j)
                                     : (hole < home || home <= j);
            if (!between) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmpty;
        --size_;
        return true;
    }

    size_t size() const { return size_; }

private:
    struct Slot {
        uint64_t key;
        uint32_t value;
    };

    // Order ids are handed out in increasing order, so using the id itself as
    // the hash puts orders that arrived close together in neighbouring slots.
    // Adds, fills and cancels mostly touch recent orders, so they hit memory
    // that is already in cache. This was the Day 6 profiling fix: with a
    // mixing hash (splitmix64) every lookup landed on a random cache line and
    // the id lookup in addOrder was ~30% of all L1 read misses.
    // Downside: ids that are all multiples of a big power of two would pile
    // into the same slots. Fine for exchange-assigned ids, not for arbitrary ones.
    // Build with -DORDERBOOK_MIXED_HASH=ON to get the old behaviour back.
#ifdef ORDERBOOK_MIXED_HASH
    static uint64_t hash(uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }
#else
    static uint64_t hash(uint64_t x) { return x; }
#endif

    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, Slot{kEmpty, 0});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const Slot& s : old)
            if (s.key != kEmpty) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    size_t size_ = 0;
};
