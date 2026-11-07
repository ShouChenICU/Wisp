#pragma once

// Containers and buffers whose storage lives in Mem::Large (PSRAM on the
// ESP port). With PSRAM enabled, an ordinary malloc/new smaller than
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) prefers internal RAM, and so
// does every intermediate buffer a std::string/std::vector reallocates
// through while it grows (AGENTS.md section 8). These types route every
// allocation to os::alloc(Mem::Large) from the first byte.
//
// Header-only on top of os::alloc/dealloc, so it carries the same
// portability rule as os.hpp: no platform headers. It also cannot log:
// WLOG_* lives in wisp_core, a higher layer. A caller that wants a failed
// allocation logged checks (LargeBuffer::ok(), a null LargeUnique) and
// logs itself.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "wisp/os/os.hpp"

namespace wisp::os {

// An owned, fixed-capacity byte buffer in Mem::Large. Move-only. A failed
// allocate() yields a buffer with !ok() and size() == 0, which is also
// the state of a default-constructed or moved-from buffer.
class LargeBuffer {
public:
    LargeBuffer() = default;

    // allocate(0) succeeds with an empty buffer: an empty payload (e.g. an
    // HTTP response with no body) is not an allocation failure.
    static LargeBuffer allocate(size_t size) {
        LargeBuffer b;
        b.data_ = static_cast<uint8_t*>(alloc(size == 0 ? 1 : size, Mem::Large));
        if (b.data_ != nullptr) {
            b.size_ = size;
        }
        return b;
    }

    ~LargeBuffer() { dealloc(data_); }

    LargeBuffer(LargeBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

    LargeBuffer& operator=(LargeBuffer&& other) noexcept {
        if (this != &other) {
            dealloc(data_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    LargeBuffer(const LargeBuffer&) = delete;
    LargeBuffer& operator=(const LargeBuffer&) = delete;

    bool ok() const { return data_ != nullptr; }
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

    // Shrinks the visible size (e.g. to the bytes actually received)
    // without reallocating. A larger `n` than size() is clamped.
    void truncate(size_t n) { size_ = n < size_ ? n : size_; }

    std::span<uint8_t> span() { return {data_, size_}; }
    std::span<const uint8_t> span() const { return {data_, size_}; }

private:
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

// A standard allocator over Mem::Large. Without exceptions there is no
// std::bad_alloc to throw, and the standard containers cannot report a
// failed allocation any other way, so running out of PSRAM aborts. Size
// the data before handing it to a container when that matters.
template <class T>
struct LargeAllocator {
    using value_type = T;

    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "os::alloc only guarantees malloc's alignment");

    LargeAllocator() = default;
    template <class U>
    LargeAllocator(const LargeAllocator<U>&) noexcept {}

    T* allocate(size_t n) {
        if (n > std::numeric_limits<size_t>::max() / sizeof(T)) {
            std::abort();
        }
        void* p = alloc(n * sizeof(T), Mem::Large);
        if (p == nullptr) {
            std::abort();
        }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_t /*n*/) noexcept { dealloc(p); }

    template <class U>
    bool operator==(const LargeAllocator<U>&) const noexcept {
        return true;
    }
};

using LargeString = std::basic_string<char, std::char_traits<char>, LargeAllocator<char>>;

template <class T>
using LargeVector = std::vector<T, LargeAllocator<T>>;

template <class T>
struct LargeDeleter {
    void operator()(T* p) const noexcept {
        if (p != nullptr) {
            p->~T();
            dealloc(p);
        }
    }
};

template <class T>
using LargeUnique = std::unique_ptr<T, LargeDeleter<T>>;

// Constructs a T in Mem::Large. Unlike LargeAllocator this does not abort:
// it returns an empty pointer when PSRAM is exhausted, so the caller can
// log and degrade.
template <class T, class... A>
LargeUnique<T> make_large(A&&... args) {
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "os::alloc only guarantees malloc's alignment");
    void* p = alloc(sizeof(T), Mem::Large);
    if (p == nullptr) {
        return LargeUnique<T>();
    }
    return LargeUnique<T>(::new (p) T(std::forward<A>(args)...));
}

// A copyable std::function look-alike whose callable always lives in
// Mem::Large. std::function keeps a callable inline only when it is
// trivially copyable and no bigger than two pointers, so a lambda holding
// a shared_ptr or a few references goes to the default heap, internal RAM
// on the device, for as long as the function lives. Use this for
// callables kept for the life of the program. Like LargeAllocator it
// aborts when PSRAM is exhausted, and calling an empty one aborts.
template <class Sig>
class LargeFunction;

template <class R, class... A>
class LargeFunction<R(A...)> {
public:
    LargeFunction() = default;
    LargeFunction(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)

    template <class F, class D = std::decay_t<F>,
              class = std::enable_if_t<!std::is_same_v<D, LargeFunction> &&
                                       !std::is_same_v<D, std::nullptr_t> &&
                                       std::is_invocable_r_v<R, D&, A...>>>
    LargeFunction(F&& f) {  // NOLINT(google-explicit-constructor)
        static_assert(alignof(D) <= alignof(std::max_align_t),
                      "os::alloc only guarantees malloc's alignment");
        obj_ = ::new (checked_alloc(sizeof(D))) D(std::forward<F>(f));
        ops_ = &kOps<D>;
    }

    LargeFunction(const LargeFunction& other)
        : obj_(other.ops_ != nullptr ? other.ops_->clone(other.obj_) : nullptr), ops_(other.ops_) {}

    LargeFunction(LargeFunction&& other) noexcept
        : obj_(std::exchange(other.obj_, nullptr)), ops_(std::exchange(other.ops_, nullptr)) {}

    LargeFunction& operator=(const LargeFunction& other) {
        if (this != &other) {
            LargeFunction copy(other);
            swap(copy);
        }
        return *this;
    }

    LargeFunction& operator=(LargeFunction&& other) noexcept {
        if (this != &other) {
            reset();
            obj_ = std::exchange(other.obj_, nullptr);
            ops_ = std::exchange(other.ops_, nullptr);
        }
        return *this;
    }

    LargeFunction& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    ~LargeFunction() { reset(); }

    explicit operator bool() const noexcept { return ops_ != nullptr; }

    R operator()(A... args) const {
        if (ops_ == nullptr) {
            std::abort();
        }
        return ops_->call(obj_, std::forward<A>(args)...);
    }

    void swap(LargeFunction& other) noexcept {
        std::swap(obj_, other.obj_);
        std::swap(ops_, other.ops_);
    }

private:
    struct Ops {
        R (*call)(void*, A&&...);
        void* (*clone)(const void*);
        void (*destroy)(void*);
    };

    static void* checked_alloc(size_t bytes) {
        void* p = alloc(bytes, Mem::Large);
        if (p == nullptr) {
            std::abort();
        }
        return p;
    }

    template <class D>
    static constexpr Ops kOps{
        [](void* o, A&&... args) -> R {
            return static_cast<R>((*static_cast<D*>(o))(std::forward<A>(args)...));
        },
        [](const void* o) -> void* {
            return ::new (checked_alloc(sizeof(D))) D(*static_cast<const D*>(o));
        },
        [](void* o) {
            static_cast<D*>(o)->~D();
            dealloc(o);
        },
    };

    void reset() noexcept {
        if (ops_ != nullptr) {
            ops_->destroy(obj_);
        }
        obj_ = nullptr;
        ops_ = nullptr;
    }

    void* obj_ = nullptr;
    const Ops* ops_ = nullptr;
};

}  // namespace wisp::os
