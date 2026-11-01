#pragma once

#include <string>
#include <utility>
#include <variant>

#include "wisp/base/error.hpp"

namespace wisp {

namespace detail {

// Carries a propagated Error out of WISP_TRY. Result<T>'s (and
// Result<void>'s) converting constructor accepts this for any T, which is
// what lets the macro's `return` statement work generically without the
// macro needing to know the enclosing function's T.
struct ErrorProxy {
    Error err;
};

}  // namespace detail

// wisp::Result<T> is the portable, exception-free way to return either a
// value of type T or a wisp::Error from a function. Internally it is a
// std::variant<T, Error>.
template <class T>
class Result {
public:
    static Result ok(T value) {
        return Result(std::variant<T, Error>(std::in_place_index<0>, std::move(value)));
    }

    static Result err(ErrorCode code, std::string message = {}) {
        return Result(
            std::variant<T, Error>(std::in_place_index<1>, Error{code, std::move(message)}));
    }

    // Converts a propagated error (from WISP_TRY) into a Result<T>.
    Result(const detail::ErrorProxy& proxy)  // NOLINT(google-explicit-constructor)
        : storage_(std::in_place_index<1>, proxy.err) {}

    bool is_ok() const { return storage_.index() == 0; }
    explicit operator bool() const { return is_ok(); }

    T& value() { return std::get<0>(storage_); }
    const T& value() const { return std::get<0>(storage_); }

    T value_or(T fallback) const {
        if (is_ok()) {
            return std::get<0>(storage_);
        }
        return fallback;
    }

    const Error& error() const { return std::get<1>(storage_); }

private:
    explicit Result(std::variant<T, Error> storage) : storage_(std::move(storage)) {}

    std::variant<T, Error> storage_;
};

// Specialization for functions that either succeed with no value or fail
// with an Error.
template <>
class Result<void> {
public:
    static Result ok() { return Result(true, Error{}); }

    static Result err(ErrorCode code, std::string message = {}) {
        return Result(false, Error{code, std::move(message)});
    }

    // Converts a propagated error (from WISP_TRY) into a Result<void>.
    Result(const detail::ErrorProxy& proxy)  // NOLINT(google-explicit-constructor)
        : ok_(false), error_(proxy.err) {}

    bool is_ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    const Error& error() const { return error_; }

private:
    Result(bool ok, Error error) : ok_(ok), error_(std::move(error)) {}

    bool ok_;
    Error error_;
};

// WISP_TRY(expr): `expr` must evaluate to a Result<V> (for any V). If it
// holds an error, WISP_TRY immediately returns that error (same code and
// message) from the enclosing function, converted to the enclosing
// function's own Result<U> return type. Otherwise execution continues.
//
// Usage (inside a function returning Result<U>):
//   WISP_TRY(some_call_returning_result());
#define WISP_TRY(expr)                                                   \
    do {                                                                 \
        auto&& wisp_try_result_ = (expr);                                \
        if (!wisp_try_result_) {                                         \
            return ::wisp::detail::ErrorProxy{wisp_try_result_.error()}; \
        }                                                                \
    } while (0)

}  // namespace wisp
