#include "completion_box.hpp"

#include <mutex>
#include <utility>

namespace wisp::scene {

std::shared_ptr<CompletionBox> CompletionBox::make() {
    // The box (its ring) is the allocation that can fail gracefully; only
    // the shared count's small block goes through the aborting allocator,
    // like every other allocate_shared in Mem::Large.
    os::LargeUnique<CompletionBox> box = os::make_large<CompletionBox>();
    if (!box) return nullptr;
    return std::shared_ptr<CompletionBox>(box.release(), os::LargeDeleter<CompletionBox>(),
                                          os::LargeAllocator<CompletionBox>());
}

bool CompletionBox::put(uint32_t id, bool files, Payload p, bool* wake) {
    *wake = false;
    // Made before the lock and, when refused, freed after it (declared
    // before the guard).
    os::LargeUnique<Payload> payload = os::make_large<Payload>(std::move(p));
    std::lock_guard<os::Mutex> lock(mu_);
    if (!open_ || size_ == kCapacity) return false;
    Item& item = ring_[(head_ + size_) % kCapacity];
    item.id = id;
    item.files = files;
    item.payload = std::move(payload);
    ++size_;
    *wake = !wake_sent_;
    wake_sent_ = true;
    return true;
}

void CompletionBox::wake_failed() {
    std::lock_guard<os::Mutex> lock(mu_);
    wake_sent_ = false;
}

bool CompletionBox::take(Item& out) {
    // `out`'s old payload is freed after the lock is let go.
    Item old = std::move(out);
    std::lock_guard<os::Mutex> lock(mu_);
    if (size_ == 0) return false;
    out = std::move(ring_[head_]);
    head_ = (head_ + 1) % kCapacity;
    --size_;
    if (size_ == 0) wake_sent_ = false;
    return true;
}

bool CompletionBox::empty() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return size_ == 0;
}

bool CompletionBox::closed() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return !open_;
}

size_t CompletionBox::close() {
    // Freed after the lock is let go (declared before the guard).
    std::array<Item, kCapacity> dropped;
    std::lock_guard<os::Mutex> lock(mu_);
    open_ = false;
    const size_t n = size_;
    for (size_t i = 0; i < n; ++i) dropped[i] = std::move(ring_[(head_ + i) % kCapacity]);
    head_ = 0;
    size_ = 0;
    return n;
}

}  // namespace wisp::scene
