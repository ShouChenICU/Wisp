#include "wisp/scene/scene_schedule.hpp"

#include <algorithm>

namespace wisp::scene {

std::vector<Eviction> pick_evictions(std::span<const SlotInfo> slots, bool opening,
                                     size_t max_background, size_t budget,
                                     size_t foreground_reserve) {
    std::vector<const SlotInfo*> back;
    bool front = opening;
    for (const SlotInfo& s : slots) {
        if (s.foreground)
            front = true;
        else
            back.push_back(&s);
    }
    std::stable_sort(back.begin(), back.end(), [](const SlotInfo* a, const SlotInfo* b) {
        return a->last_front_ms < b->last_front_ms;
    });

    std::vector<Eviction> out;
    size_t next = 0;
    while (back.size() - next > max_background) out.push_back({back[next++]->id, false});
    if (!front) return out;

    const size_t room = budget > foreground_reserve ? budget - foreground_reserve : 0;
    size_t bytes = 0;
    for (size_t i = next; i < back.size(); ++i) bytes += back[i]->vm_bytes;
    while (next < back.size() && bytes > room) {
        bytes -= back[next]->vm_bytes;
        out.push_back({back[next++]->id, true});
    }
    return out;
}

}  // namespace wisp::scene
