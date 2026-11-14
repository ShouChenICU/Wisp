#pragma once

// A state cell: a value other components follow (spec 3.3 of the
// state-cells design). The owning component writes it with set(),
// update() or seed(); anyone reads it with get() or read() and follows it
// with subscribe().
//
// Delivery. A subscription's callback runs on the dispatcher it was
// given (wisp.bus by default, the UiRuntime for UI code), never inside
// set() and never on the setter's thread unless that thread is the
// dispatcher. Its first call delivers the current value; after that a
// call means "changed since your last call": values set while it was
// queued or running are skipped, and the next call reads whatever is
// current then. A -> B -> A between two calls therefore delivers A again,
// so callbacks must be idempotent. Calls of one subscription never
// overlap and come in revision order; the last change is always followed
// by one call that sees it, however late its dispatcher runs.
//
// Locks. Each cell has one lock, held only for memory work: copying the
// value, the intrusive list of subscriptions, read()'s body. While it is
// held a cell calls out only to (a) its mirror, which may only call
// EventBus::publish() (publish_mutex_ and a non-blocking push), and (b)
// Dispatcher::schedule(), which only takes the ready list's lock. The
// order is therefore cell -> bus publish_mutex_ and cell -> ready list,
// and no path takes them the other way: a dispatcher runs a slot, and
// calls finish(), with its ready-list lock released. No callback runs
// while any cell's lock is held. set() allocates nothing.
//
// Re-entry. From a callback a subscriber may get()/read() any cell, set()
// any cell (its own included: the subscription runs again, so a callback
// that always sets a new value loops, which is a bug), subscribe(), and
// release any subscription, its own included. From inside a cell's own
// read() body, update() mutator or mirror it may not touch that cell
// again: its lock is not recursive (host builds report this as misuse).
//
// Callbacks run on wisp.bus or the UI task, neither of which may touch
// flash (AGENTS.md section 5): hand flash work to StorageWorker.
//
// Memory. The cell lives inside its owner; its lock is the one object of
// its own in internal RAM. Subscriptions and their callbacks live in
// Mem::Large.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <utility>

#include "wisp/core/dispatcher.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

class CellSubscription;
template <class T>
class StateCell;

// Not `detail`: a wisp::core::detail would hide wisp::detail for every
// file that includes this header.
namespace cell_detail {

class CellCore;

// One subscription. The dispatcher owns the DispatchSlot fields; the
// list links are guarded by the cell's lock; `delivered` is touched only
// by runs, which never overlap. `finish` doubles as the subscription's
// typed deleter.
struct CellSub : DispatchSlot {
    // Who tears the subscription down. Its handle sets kHandleGone when it
    // is released; a cell destroyed with the subscription still linked
    // (misuse, reported) takes it over with kCellOwned and sets kCellDone
    // when it no longer touches it. Whichever of the handle and the cell
    // comes second frees it, so neither waits for the other and the slot
    // is cancelled by exactly one of them.
    static constexpr uint8_t kHandleGone = 1, kCellOwned = 2, kCellDone = 4;

    CellSub* prev_sub = nullptr;
    CellSub* next_sub = nullptr;
    CellCore* core = nullptr;
    Dispatcher* on = nullptr;
    uint32_t delivered = 0;  // revision of the last call; 0 = none yet
    std::atomic<uint8_t> claim{0};
};

// The part of a cell that does not depend on T.
class CellCore {
public:
    CellCore(const CellCore&) = delete;
    CellCore& operator=(const CellCore&) = delete;

    // CellSubscription::reset(). Unlinks `sub` under the cell's lock, so no
    // set() schedules it again, then cancels its slot and frees it (or
    // leaves that to DispatchSlot::finish when called from inside its own
    // run).
    static void release(CellSub* sub);

protected:
    explicit CellCore(EventBus& bus) : bus_(bus) {}
    ~CellCore() = default;

    // Holds the cell's lock. Host builds also note, per thread, which cell
    // the thread holds, so taking it again from inside read(), update()'s
    // mutator or the mirror is reported instead of deadlocking.
    class Hold {
    public:
        explicit Hold(const CellCore& core);
        ~Hold() { unlock(); }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        void unlock();

    private:
        const CellCore* core_;
#if !defined(ESP_PLATFORM)
        const CellCore* outer_ = nullptr;
#endif
    };

    // Links `sub`, attaches it to `on`, counts it on the bus and schedules
    // its first run, all under the lock.
    void link(CellSub& sub, Dispatcher& on) const;
    // Under the lock: the value has changed.
    void changed_locked();
    // From the derived destructor, while the value still exists: reports
    // live subscriptions as misuse, then (device builds, where misuse
    // returns) detaches each so a later release() only frees it.
    void release_all();

    EventBus& bus_;
    std::atomic<uint32_t> revision_{1};

private:
    void unlink_locked(CellSub& sub) const;
    void orphan(CellSub& sub);

    mutable os::Mutex mu_;
    mutable CellSub* head_ = nullptr;
    mutable CellSub* tail_ = nullptr;
    // Subscriptions a release() has unlinked but not yet cancelled: their
    // run may still be about to take this cell's lock.
    mutable std::atomic<uint32_t> releasing_{0};
};

}  // namespace cell_detail

// Move-only. reset() or destruction unsubscribes: the subscription leaves
// the cell's list (no set() schedules it again), then its slot is
// cancelled. Off the dispatcher's thread that waits for this
// subscription's callback in progress (only this one), so once it returns
// the callback never runs again; inside its own callback it returns at
// once, no further call happens, and the memory goes when the run returns
// (DispatchSlot::finish). Must not be called while holding a lock the
// callback takes. One handle is one owner: two threads must not reset the
// same handle at once, as with any object.
class CellSubscription {
public:
    CellSubscription() = default;
    CellSubscription(CellSubscription&& other) noexcept
        : sub_(std::exchange(other.sub_, nullptr)) {}
    // Releases the subscription this handle held first.
    CellSubscription& operator=(CellSubscription&& other) noexcept {
        if (this != &other) {
            reset();
            sub_ = std::exchange(other.sub_, nullptr);
        }
        return *this;
    }
    CellSubscription(const CellSubscription&) = delete;
    CellSubscription& operator=(const CellSubscription&) = delete;
    ~CellSubscription() { reset(); }

    void reset() {
        // Cleared first, so a callback resetting this same handle again
        // finds it empty.
        if (cell_detail::CellSub* sub = std::exchange(sub_, nullptr)) {
            cell_detail::CellCore::release(sub);
        }
    }
    explicit operator bool() const { return sub_ != nullptr; }

private:
    template <class T>
    friend class StateCell;
    explicit CellSubscription(cell_detail::CellSub* sub) : sub_(sub) {}

    cell_detail::CellSub* sub_ = nullptr;
};

template <class T>
class StateCell : private cell_detail::CellCore {
    static_assert(std::is_copy_constructible_v<T>);
    static_assert(sizeof(T) <= 256, "a callback copies T onto the dispatcher's stack");

public:
    using Callback = os::LargeFunction<void(const T&)>;
    // Publishes the legacy bus event(s) for a change made by set() or
    // update(); called under the cell's lock, so it may only publish.
    using Mirror = void (*)(EventBus& bus, const T& before, const T& after);

    // `bus` runs the subscriptions that name no dispatcher and counts them
    // all; it must outlive the cell.
    StateCell(EventBus& bus, T initial, Mirror mirror = nullptr)
        : CellCore(bus), value_(std::move(initial)), mirror_(mirror) {}
    // Every subscription must be released first; a live one is misuse
    // ("StateCell destroyed with N live subscriptions").
    ~StateCell() { release_all(); }

    StateCell(const StateCell&) = delete;
    StateCell& operator=(const StateCell&) = delete;

    // --- readers: any thread ---------------------------------------------
    T get() const {
        Hold hold(*this);
        return value_;
    }
    // f(const T&) under the cell's lock: keep it short, call nothing out.
    template <class F>
    auto read(F&& f) const -> std::invoke_result_t<F&, const T&> {
        Hold hold(*this);
        return f(value_);
    }
    // Starts at 1 and moves on every change; wraps, so compare for
    // equality only.
    uint32_t revision() const { return revision_.load(std::memory_order_acquire); }

    // Calls `cb` with the current value soon, then after every change (see
    // the file comment), on `on` (nullptr: the cell's bus). `on` must
    // outlive the subscription.
    [[nodiscard]] CellSubscription subscribe(Callback cb, Dispatcher* on = nullptr) const {
        os::LargeUnique<Sub> sub = os::make_large<Sub>();
        if (!sub) {
            // As os::LargeAllocator: a subscription has no way to fail.
            std::abort();
        }
        sub->run = &StateCell::run;
        sub->finish = &StateCell::destroy;
        sub->callback = std::move(cb);
        Sub* raw = sub.release();
        link(*raw, on != nullptr ? *on : static_cast<Dispatcher&>(bus_));
        return CellSubscription(raw);
    }

    // --- owner only ------------------------------------------------------
    // false, and nothing happens, when v == the current value.
    bool set(const T& v) {
        Hold hold(*this);
        if (value_ == v) {
            return false;
        }
        store_locked(v, mirror_);
        return true;
    }
    // mutate(T&) on a copy under the lock, then as set().
    template <class F>
    bool update(F&& mutate) {
        Hold hold(*this);
        T next(value_);
        mutate(next);
        if (next == value_) {
            return false;
        }
        store_locked(next, mirror_);
        return true;
    }
    // As set() without the mirror: for a first value that is not a change
    // anyone published before.
    bool seed(const T& v) {
        Hold hold(*this);
        if (value_ == v) {
            return false;
        }
        store_locked(v, nullptr);
        return true;
    }

private:
    struct Sub : cell_detail::CellSub {
        Callback callback;
    };

    void store_locked(const T& next, Mirror mirror) {
        if (mirror != nullptr) {
            T before(value_);
            value_ = next;
            changed_locked();
            mirror(bus_, before, value_);
        } else {
            value_ = next;
            changed_locked();
        }
    }

    static void run(DispatchSlot& slot) {
        Sub& sub = static_cast<Sub&>(slot);
        const StateCell& cell = static_cast<const StateCell&>(*sub.core);
        Hold hold(cell);
        uint32_t revision = cell.revision_.load(std::memory_order_relaxed);
        if (revision == sub.delivered) {
            return;
        }
        T copy(cell.value_);
        hold.unlock();
        sub.delivered = revision;
        // The last touch of the subscription and the cell: the callback may
        // release the subscription (then finish frees it after this run)
        // or, from elsewhere, free things the cell is part of.
        sub.callback(copy);
    }

    static void destroy(DispatchSlot& slot) { os::LargeDeleter<Sub>{}(static_cast<Sub*>(&slot)); }

    T value_;
    const Mirror mirror_;
};

}  // namespace wisp::core
