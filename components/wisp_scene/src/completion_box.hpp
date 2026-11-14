#pragma once

// One VM's finished wisp.http / wisp.files calls waiting for the scene task
// (scene_runtime.hpp, "Results"). Producers: the wisp.shttp workers, the
// wisp.fs worker and the scene task itself (refusals made inside a call).
// Made once per VM (make()) and shared with every deliver lambda, so a
// late producer never touches a freed box, and a result always goes to the
// box of the instance that made the call, never to a later instance of the
// same scene.
//
// Bounded: a VM never has more than kMaxCallsInFlight calls waiting
// (scene_api.hpp), and each holds its place until its result has run, so
// the ring never needs more entries. It is allocated with the box; a put
// allocates only the payload's holder, and keeps the call (with no
// payload) when even that fails, so the callback still runs once.
//
// Locking: `mu_` is a leaf lock. Nothing else is locked, and nothing that
// could block or allocate is called, while it is held; payloads are made
// before it is taken and freed after it is let go. So a producer never
// waits for Lua running, and it may put while holding nothing or before it
// takes SceneRuntime::Link::mu (lock order: Link::mu, then the scene
// queue's own lock; `mu_` is never held with either).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

#include "scene_api.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::scene {

class CompletionBox {
public:
    static constexpr size_t kCapacity = kMaxCallsInFlight;
    using Payload = std::variant<net::SceneHttpResult, FilesResult>;
    struct Item {
        uint32_t id = 0;                   // the binding's callback id
        bool files = false;                // which callback table (and which no-memory answer)
        os::LargeUnique<Payload> payload;  // null: no memory to keep the result
    };

    // A new open box in Mem::Large; null without the memory for it.
    static std::shared_ptr<CompletionBox> make();

    // Any thread. Moves `p` into a make_large<Payload>; on allocation
    // failure keeps the item with a null payload. False (and `p` dropped)
    // when closed or full. *wake is set when the caller must send the
    // scene task a wake-up: the box was not already waiting for one.
    bool put(uint32_t id, bool files, Payload p, bool* wake);
    // Any thread: the wake-up could not be queued; the next put() asks again.
    void wake_failed();
    // Scene task. Oldest first; taking the last item clears the wake flag.
    bool take(Item& out);
    bool empty() const;
    // Any thread. Once true it stays true, so a put() that failed on a box
    // that is open afterwards failed because the box was full.
    bool closed() const;
    // Scene task. No put() succeeds afterwards; returns how many items
    // were waiting (they are freed here).
    size_t close();

private:
    mutable os::Mutex mu_;
    bool open_ = true;
    bool wake_sent_ = false;
    std::array<Item, kCapacity> ring_;
    size_t head_ = 0, size_ = 0;
};

}  // namespace wisp::scene
