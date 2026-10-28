#ifndef ROS2_BAG_RESULT_HPP_
#define ROS2_BAG_RESULT_HPP_

#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace ros2_bag {

struct unexpect_t {
    explicit constexpr unexpect_t() = default;
};

inline constexpr unexpect_t unexpect{};

template <class E>
class Unexpected;

template <class T, class E>
class [[nodiscard]] Result;

namespace result_traits {

template <class T>
using RemoveCvRefT = std::remove_cv_t<std::remove_reference_t<T>>;

template <class...>
using VoidT = void;

template <class T, class = void>
struct IsComplete : std::false_type {};

template <class T>
struct IsComplete<T, VoidT<decltype(sizeof(T))>> : std::true_type {};

template <class T>
inline constexpr bool kIsComplete = IsComplete<T>::value;

template <class T>
struct IsUnexpected : std::false_type {};

template <class E>
struct IsUnexpected<Unexpected<E>> : std::true_type {};

template <class T>
inline constexpr bool kIsUnexpected = IsUnexpected<RemoveCvRefT<T>>::value;

template <class T>
struct IsResult : std::false_type {};

template <class T, class E>
struct IsResult<Result<T, E>> : std::true_type {};

template <class T>
inline constexpr bool kIsResult = IsResult<RemoveCvRefT<T>>::value;

template <
    class T,
    bool = kIsComplete<T>&& std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> &&
           !std::is_volatile_v<T> && !kIsUnexpected<T>>
struct IsValidErrorType : std::false_type {};

template <class T>
struct IsValidErrorType<T, true>
: std::bool_constant<!std::is_abstract_v<T> && std::is_nothrow_destructible_v<T>> {};

template <
    class T,
    bool = kIsComplete<T>&& std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> &&
           !std::is_volatile_v<T> && !std::is_same_v<T, std::in_place_t> &&
           !std::is_same_v<T, unexpect_t> && !kIsUnexpected<T>>
struct IsValidValueType : std::false_type {};

template <class T>
struct IsValidValueType<T, true>
: std::bool_constant<!std::is_abstract_v<T> && std::is_nothrow_destructible_v<T>> {};

}  // namespace result_traits

namespace result_storage {

[[noreturn]] inline void terminate_invalid_storage_state() noexcept { std::terminate(); }

enum class ActiveAlternative : unsigned char { Empty, Value, Error };

struct EmptyStorageTag {
    explicit constexpr EmptyStorageTag() = default;
};

inline constexpr EmptyStorageTag kEmptyStorage{};

template <class T, class E, bool kTrivialDestructor>
union ValueOrErrorUnion;

template <class T, class E>
union ValueOrErrorUnion<T, E, true> {
    T value_;
    E error_;

    ValueOrErrorUnion() noexcept {}

    template <class... Args>
    constexpr explicit ValueOrErrorUnion(std::in_place_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>)
    : value_(std::forward<Args>(args)...) {}

    template <class... Args>
    constexpr explicit ValueOrErrorUnion(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : error_(std::forward<Args>(args)...) {}

    ValueOrErrorUnion(const ValueOrErrorUnion&) = default;
    ValueOrErrorUnion(ValueOrErrorUnion&&) = default;
    ValueOrErrorUnion& operator=(const ValueOrErrorUnion&) = default;
    ValueOrErrorUnion& operator=(ValueOrErrorUnion&&) = default;
    ~ValueOrErrorUnion() = default;
};

template <class T, class E>
union ValueOrErrorUnion<T, E, false> {
    T value_;
    E error_;

    ValueOrErrorUnion() noexcept {}

    template <class... Args>
    explicit ValueOrErrorUnion(std::in_place_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>)
    : value_(std::forward<Args>(args)...) {}

    template <class... Args>
    explicit ValueOrErrorUnion(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : error_(std::forward<Args>(args)...) {}

    ValueOrErrorUnion(const ValueOrErrorUnion&) = default;
    ValueOrErrorUnion(ValueOrErrorUnion&&) = default;
    ValueOrErrorUnion& operator=(const ValueOrErrorUnion&) = default;
    ValueOrErrorUnion& operator=(ValueOrErrorUnion&&) = default;

    ~ValueOrErrorUnion() {}
};

template <class E, bool kTrivialDestructor>
union SuccessOrErrorUnion;

template <class E>
union SuccessOrErrorUnion<E, true> {
    unsigned char success_marker_;
    E error_;

    constexpr SuccessOrErrorUnion() noexcept : success_marker_(0) {}

    template <class... Args>
    constexpr explicit SuccessOrErrorUnion(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : error_(std::forward<Args>(args)...) {}

    SuccessOrErrorUnion(const SuccessOrErrorUnion&) = default;
    SuccessOrErrorUnion(SuccessOrErrorUnion&&) = default;
    SuccessOrErrorUnion& operator=(const SuccessOrErrorUnion&) = default;
    SuccessOrErrorUnion& operator=(SuccessOrErrorUnion&&) = default;
    ~SuccessOrErrorUnion() = default;
};

template <class E>
union SuccessOrErrorUnion<E, false> {
    unsigned char success_marker_;
    E error_;

    SuccessOrErrorUnion() noexcept : success_marker_(0) {}

    template <class... Args>
    explicit SuccessOrErrorUnion(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : error_(std::forward<Args>(args)...) {}

    SuccessOrErrorUnion(const SuccessOrErrorUnion&) = default;
    SuccessOrErrorUnion(SuccessOrErrorUnion&&) = default;
    SuccessOrErrorUnion& operator=(const SuccessOrErrorUnion&) = default;
    SuccessOrErrorUnion& operator=(SuccessOrErrorUnion&&) = default;

    ~SuccessOrErrorUnion() {}
};

template <class T, class E>
class ValueOrErrorStorage {
protected:
    static constexpr bool kTrivialDestructor =
        std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<E>;

    using StorageUnion = ValueOrErrorUnion<T, E, kTrivialDestructor>;

    explicit ValueOrErrorStorage(EmptyStorageTag) noexcept
    : storage_(), state_(ActiveAlternative::Empty) {}

    template <class... Args>
    constexpr explicit ValueOrErrorStorage(std::in_place_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>)
    : storage_(std::in_place, std::forward<Args>(args)...), state_(ActiveAlternative::Value) {}

    template <class... Args>
    constexpr explicit ValueOrErrorStorage(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : storage_(unexpect, std::forward<Args>(args)...), state_(ActiveAlternative::Error) {}

    ValueOrErrorStorage(const ValueOrErrorStorage&) = default;
    ValueOrErrorStorage(ValueOrErrorStorage&&) = default;
    ValueOrErrorStorage& operator=(const ValueOrErrorStorage&) = default;
    ValueOrErrorStorage& operator=(ValueOrErrorStorage&&) = default;
    ~ValueOrErrorStorage() = default;

    constexpr bool stores_value() const noexcept { return state_ == ActiveAlternative::Value; }

    constexpr bool stores_error() const noexcept { return state_ == ActiveAlternative::Error; }

    constexpr T& value_ref() & noexcept { return storage_.value_; }

    constexpr const T& value_ref() const& noexcept { return storage_.value_; }

    constexpr T&& value_ref() && noexcept { return std::move(storage_.value_); }

    constexpr const T&& value_ref() const&& noexcept { return std::move(storage_.value_); }

    constexpr E& error_ref() & noexcept { return storage_.error_; }

    constexpr const E& error_ref() const& noexcept { return storage_.error_; }

    constexpr E&& error_ref() && noexcept { return std::move(storage_.error_); }

    constexpr const E&& error_ref() const&& noexcept { return std::move(storage_.error_); }

    template <class... Args>
    void construct_value(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
        ::new (static_cast<void*>(std::addressof(storage_.value_))) T(std::forward<Args>(args)...);
        state_ = ActiveAlternative::Value;
    }

    template <class... Args>
    void construct_error(Args&&... args) noexcept(std::is_nothrow_constructible_v<E, Args&&...>) {
        ::new (static_cast<void*>(std::addressof(storage_.error_))) E(std::forward<Args>(args)...);
        state_ = ActiveAlternative::Error;
    }

    void destroy_value() noexcept {
        storage_.value_.~T();
        state_ = ActiveAlternative::Empty;
    }

    void destroy_error() noexcept {
        storage_.error_.~E();
        state_ = ActiveAlternative::Empty;
    }

    void destroy_active() noexcept {
        if (state_ == ActiveAlternative::Value) {
            destroy_value();
        } else if (state_ == ActiveAlternative::Error) {
            destroy_error();
        }
    }

    template <class... Args>
    void reinit_value(Args&&... args) {
        if constexpr (std::is_nothrow_constructible_v<T, Args&&...>) {
            destroy_error();
            construct_value(std::forward<Args>(args)...);
        } else if constexpr (std::is_nothrow_move_constructible_v<T>) {
            T temporary(std::forward<Args>(args)...);
            destroy_error();
            construct_value(std::move(temporary));
        } else {
            static_assert(
                std::is_nothrow_move_constructible_v<E>,
                "Result cannot safely reinitialize "
                "Error to Value");

            E backup(std::move(storage_.error_));
            destroy_error();

            try {
                construct_value(std::forward<Args>(args)...);
            } catch (...) {
                construct_error(std::move(backup));
                throw;
            }
        }
    }

    template <class... Args>
    void reinit_error(Args&&... args) {
        if constexpr (std::is_nothrow_constructible_v<E, Args&&...>) {
            destroy_value();
            construct_error(std::forward<Args>(args)...);
        } else if constexpr (std::is_nothrow_move_constructible_v<E>) {
            E temporary(std::forward<Args>(args)...);
            destroy_value();
            construct_error(std::move(temporary));
        } else {
            static_assert(
                std::is_nothrow_move_constructible_v<T>,
                "Result cannot safely reinitialize "
                "Value to Error");

            T backup(std::move(storage_.value_));
            destroy_value();

            try {
                construct_error(std::forward<Args>(args)...);
            } catch (...) {
                construct_value(std::move(backup));
                throw;
            }
        }
    }

    template <class... Args>
    T& emplace_value(Args&&... args) noexcept {
        destroy_active();
        construct_value(std::forward<Args>(args)...);
        return storage_.value_;
    }

    void copy_construct_from(const ValueOrErrorStorage& other) {
        if (other.state_ == ActiveAlternative::Value) {
            construct_value(other.storage_.value_);
            return;
        }
        if (other.state_ == ActiveAlternative::Error) {
            construct_error(other.storage_.error_);
            return;
        }
        terminate_invalid_storage_state();
    }

    void move_construct_from(ValueOrErrorStorage&& other) {
        if (other.state_ == ActiveAlternative::Value) {
            construct_value(std::move(other.storage_.value_));
            return;
        }
        if (other.state_ == ActiveAlternative::Error) {
            construct_error(std::move(other.storage_.error_));
            return;
        }
        terminate_invalid_storage_state();
    }

    void copy_assign_from(const ValueOrErrorStorage& other) {
        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Value) {
            storage_.value_ = other.storage_.value_;
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Error) {
            storage_.error_ = other.storage_.error_;
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Value) {
            reinit_value(other.storage_.value_);
            return;
        }

        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Error) {
            reinit_error(other.storage_.error_);
            return;
        }

        terminate_invalid_storage_state();
    }

    void move_assign_from(ValueOrErrorStorage&& other) {
        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Value) {
            storage_.value_ = std::move(other.storage_.value_);
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Error) {
            storage_.error_ = std::move(other.storage_.error_);
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Value) {
            reinit_value(std::move(other.storage_.value_));
            return;
        }

        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Error) {
            reinit_error(std::move(other.storage_.error_));
            return;
        }

        terminate_invalid_storage_state();
    }

private:
    StorageUnion storage_;
    ActiveAlternative state_;
};

template <class E>
class SuccessOrErrorStorage {
protected:
    static constexpr bool kTrivialDestructor = std::is_trivially_destructible_v<E>;

    using StorageUnion = SuccessOrErrorUnion<E, kTrivialDestructor>;

    explicit SuccessOrErrorStorage(EmptyStorageTag) noexcept
    : storage_(), state_(ActiveAlternative::Empty) {}

    explicit constexpr SuccessOrErrorStorage(std::in_place_t) noexcept
    : storage_(), state_(ActiveAlternative::Value) {}

    template <class... Args>
    constexpr explicit SuccessOrErrorStorage(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : storage_(unexpect, std::forward<Args>(args)...), state_(ActiveAlternative::Error) {}

    SuccessOrErrorStorage(const SuccessOrErrorStorage&) = default;
    SuccessOrErrorStorage(SuccessOrErrorStorage&&) = default;
    SuccessOrErrorStorage& operator=(const SuccessOrErrorStorage&) = default;
    SuccessOrErrorStorage& operator=(SuccessOrErrorStorage&&) = default;
    ~SuccessOrErrorStorage() = default;

    constexpr bool stores_value() const noexcept { return state_ == ActiveAlternative::Value; }

    constexpr bool stores_error() const noexcept { return state_ == ActiveAlternative::Error; }

    constexpr E& error_ref() & noexcept { return storage_.error_; }

    constexpr const E& error_ref() const& noexcept { return storage_.error_; }

    constexpr E&& error_ref() && noexcept { return std::move(storage_.error_); }

    constexpr const E&& error_ref() const&& noexcept { return std::move(storage_.error_); }

    template <class... Args>
    void construct_error(Args&&... args) noexcept(std::is_nothrow_constructible_v<E, Args&&...>) {
        ::new (static_cast<void*>(std::addressof(storage_.error_))) E(std::forward<Args>(args)...);
        state_ = ActiveAlternative::Error;
    }

    void activate_success() noexcept {
        ::new (static_cast<void*>(std::addressof(storage_.success_marker_))) unsigned char(0);
        state_ = ActiveAlternative::Value;
    }

    void destroy_error() noexcept {
        storage_.error_.~E();
        activate_success();
    }

    template <class... Args>
    void reinit_error_from_success(Args&&... args) {
        try {
            construct_error(std::forward<Args>(args)...);
        } catch (...) {
            activate_success();
            throw;
        }
    }

    void destroy_active() noexcept {
        if (state_ == ActiveAlternative::Error) {
            storage_.error_.~E();
        }
        state_ = ActiveAlternative::Empty;
    }

    void emplace_success() noexcept {
        if (state_ == ActiveAlternative::Error) {
            destroy_error();
        } else {
            state_ = ActiveAlternative::Value;
        }
    }

    void copy_construct_from(const SuccessOrErrorStorage& other) {
        if (other.state_ == ActiveAlternative::Value) {
            state_ = ActiveAlternative::Value;
            return;
        }
        if (other.state_ == ActiveAlternative::Error) {
            construct_error(other.storage_.error_);
            return;
        }
        terminate_invalid_storage_state();
    }

    void move_construct_from(SuccessOrErrorStorage&& other) {
        if (other.state_ == ActiveAlternative::Value) {
            state_ = ActiveAlternative::Value;
            return;
        }
        if (other.state_ == ActiveAlternative::Error) {
            construct_error(std::move(other.storage_.error_));
            return;
        }
        terminate_invalid_storage_state();
    }

    void copy_assign_from(const SuccessOrErrorStorage& other) {
        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Value) {
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Error) {
            storage_.error_ = other.storage_.error_;
            return;
        }

        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Error) {
            reinit_error_from_success(other.storage_.error_);
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Value) {
            destroy_error();
            return;
        }

        terminate_invalid_storage_state();
    }

    void move_assign_from(SuccessOrErrorStorage&& other) {
        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Value) {
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Error) {
            storage_.error_ = std::move(other.storage_.error_);
            return;
        }

        if (state_ == ActiveAlternative::Value && other.state_ == ActiveAlternative::Error) {
            reinit_error_from_success(std::move(other.storage_.error_));
            return;
        }

        if (state_ == ActiveAlternative::Error && other.state_ == ActiveAlternative::Value) {
            destroy_error();
            return;
        }

        terminate_invalid_storage_state();
    }

private:
    StorageUnion storage_;
    ActiveAlternative state_;
};

}  // namespace result_storage

namespace result_special_members {

using result_storage::EmptyStorageTag;
using result_storage::kEmptyStorage;
using result_storage::SuccessOrErrorStorage;
using result_storage::ValueOrErrorStorage;

template <class Wrapped, bool kTrivial>
class ConditionalDestructor;

template <class Wrapped>
class ConditionalDestructor<Wrapped, true> : public Wrapped {
protected:
    explicit ConditionalDestructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalDestructor(const ConditionalDestructor&) = default;
    ConditionalDestructor(ConditionalDestructor&&) = default;
    ConditionalDestructor& operator=(const ConditionalDestructor&) = default;
    ConditionalDestructor& operator=(ConditionalDestructor&&) = default;
    ~ConditionalDestructor() = default;
};

template <class Wrapped>
class ConditionalDestructor<Wrapped, false> : public Wrapped {
protected:
    explicit ConditionalDestructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalDestructor(const ConditionalDestructor&) = default;
    ConditionalDestructor(ConditionalDestructor&&) = default;
    ConditionalDestructor& operator=(const ConditionalDestructor&) = default;
    ConditionalDestructor& operator=(ConditionalDestructor&&) = default;

    ~ConditionalDestructor() { this->destroy_active(); }
};

template <class Wrapped, bool kEnabled, bool kTrivial, bool kNoexceptCopy>
class ConditionalCopyConstructor;

template <class Wrapped, bool kTrivial, bool kNoexceptCopy>
class ConditionalCopyConstructor<Wrapped, false, kTrivial, kNoexceptCopy> : public Wrapped {
protected:
    explicit ConditionalCopyConstructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalCopyConstructor(const ConditionalCopyConstructor&) = delete;
    ConditionalCopyConstructor(ConditionalCopyConstructor&&) = default;
    ConditionalCopyConstructor& operator=(const ConditionalCopyConstructor&) = default;
    ConditionalCopyConstructor& operator=(ConditionalCopyConstructor&&) = default;
};

template <class Wrapped, bool kNoexceptCopy>
class ConditionalCopyConstructor<Wrapped, true, true, kNoexceptCopy> : public Wrapped {
protected:
    explicit ConditionalCopyConstructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalCopyConstructor(const ConditionalCopyConstructor&) = default;
    ConditionalCopyConstructor(ConditionalCopyConstructor&&) = default;
    ConditionalCopyConstructor& operator=(const ConditionalCopyConstructor&) = default;
    ConditionalCopyConstructor& operator=(ConditionalCopyConstructor&&) = default;
};

template <class Wrapped, bool kNoexceptCopy>
class ConditionalCopyConstructor<Wrapped, true, false, kNoexceptCopy> : public Wrapped {
protected:
    explicit ConditionalCopyConstructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalCopyConstructor(const ConditionalCopyConstructor& other) noexcept(kNoexceptCopy)
    : Wrapped(kEmptyStorage) {
        this->copy_construct_from(other);
    }

    ConditionalCopyConstructor(ConditionalCopyConstructor&&) = default;
    ConditionalCopyConstructor& operator=(const ConditionalCopyConstructor&) = default;
    ConditionalCopyConstructor& operator=(ConditionalCopyConstructor&&) = default;
};

template <class Wrapped, bool kEnabled, bool kTrivial, bool kNoexceptMove>
class ConditionalMoveConstructor;

template <class Wrapped, bool kTrivial, bool kNoexceptMove>
class ConditionalMoveConstructor<Wrapped, false, kTrivial, kNoexceptMove> : public Wrapped {
protected:
    explicit ConditionalMoveConstructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalMoveConstructor(const ConditionalMoveConstructor&) = default;
    ConditionalMoveConstructor(ConditionalMoveConstructor&&) = delete;
    ConditionalMoveConstructor& operator=(const ConditionalMoveConstructor&) = default;
    ConditionalMoveConstructor& operator=(ConditionalMoveConstructor&&) = default;
};

template <class Wrapped, bool kNoexceptMove>
class ConditionalMoveConstructor<Wrapped, true, true, kNoexceptMove> : public Wrapped {
protected:
    explicit ConditionalMoveConstructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalMoveConstructor(const ConditionalMoveConstructor&) = default;
    ConditionalMoveConstructor(ConditionalMoveConstructor&&) = default;
    ConditionalMoveConstructor& operator=(const ConditionalMoveConstructor&) = default;
    ConditionalMoveConstructor& operator=(ConditionalMoveConstructor&&) = default;
};

template <class Wrapped, bool kNoexceptMove>
class ConditionalMoveConstructor<Wrapped, true, false, kNoexceptMove> : public Wrapped {
protected:
    explicit ConditionalMoveConstructor(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalMoveConstructor(const ConditionalMoveConstructor&) = default;

    ConditionalMoveConstructor(ConditionalMoveConstructor&& other) noexcept(kNoexceptMove)
    : Wrapped(kEmptyStorage) {
        this->move_construct_from(std::move(other));
    }

    ConditionalMoveConstructor& operator=(const ConditionalMoveConstructor&) = default;
    ConditionalMoveConstructor& operator=(ConditionalMoveConstructor&&) = default;
};

template <class Wrapped, bool kEnabled, bool kTrivial, bool kNoexceptCopy>
class ConditionalCopyAssignment;

template <class Wrapped, bool kTrivial, bool kNoexceptCopy>
class ConditionalCopyAssignment<Wrapped, false, kTrivial, kNoexceptCopy> : public Wrapped {
protected:
    explicit ConditionalCopyAssignment(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalCopyAssignment(const ConditionalCopyAssignment&) = default;
    ConditionalCopyAssignment(ConditionalCopyAssignment&&) = default;
    ConditionalCopyAssignment& operator=(const ConditionalCopyAssignment&) = delete;
    ConditionalCopyAssignment& operator=(ConditionalCopyAssignment&&) = default;
};

template <class Wrapped, bool kNoexceptCopy>
class ConditionalCopyAssignment<Wrapped, true, true, kNoexceptCopy> : public Wrapped {
protected:
    explicit ConditionalCopyAssignment(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalCopyAssignment(const ConditionalCopyAssignment&) = default;
    ConditionalCopyAssignment(ConditionalCopyAssignment&&) = default;
    ConditionalCopyAssignment& operator=(const ConditionalCopyAssignment&) = default;
    ConditionalCopyAssignment& operator=(ConditionalCopyAssignment&&) = default;
};

template <class Wrapped, bool kNoexceptCopy>
class ConditionalCopyAssignment<Wrapped, true, false, kNoexceptCopy> : public Wrapped {
protected:
    explicit ConditionalCopyAssignment(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalCopyAssignment(const ConditionalCopyAssignment&) = default;
    ConditionalCopyAssignment(ConditionalCopyAssignment&&) = default;

    ConditionalCopyAssignment& operator=(const ConditionalCopyAssignment& other) noexcept(
        kNoexceptCopy) {
        if (this != std::addressof(other)) {
            this->copy_assign_from(other);
        }
        return *this;
    }

    ConditionalCopyAssignment& operator=(ConditionalCopyAssignment&&) = default;
};

template <class Wrapped, bool kEnabled, bool kTrivial, bool kNoexceptMove>
class ConditionalMoveAssignment;

template <class Wrapped, bool kTrivial, bool kNoexceptMove>
class ConditionalMoveAssignment<Wrapped, false, kTrivial, kNoexceptMove> : public Wrapped {
protected:
    explicit ConditionalMoveAssignment(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalMoveAssignment(const ConditionalMoveAssignment&) = default;
    ConditionalMoveAssignment(ConditionalMoveAssignment&&) = default;
    ConditionalMoveAssignment& operator=(const ConditionalMoveAssignment&) = default;
    ConditionalMoveAssignment& operator=(ConditionalMoveAssignment&&) = delete;
};

template <class Wrapped, bool kNoexceptMove>
class ConditionalMoveAssignment<Wrapped, true, true, kNoexceptMove> : public Wrapped {
protected:
    explicit ConditionalMoveAssignment(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalMoveAssignment(const ConditionalMoveAssignment&) = default;
    ConditionalMoveAssignment(ConditionalMoveAssignment&&) = default;
    ConditionalMoveAssignment& operator=(const ConditionalMoveAssignment&) = default;
    ConditionalMoveAssignment& operator=(ConditionalMoveAssignment&&) = default;
};

template <class Wrapped, bool kNoexceptMove>
class ConditionalMoveAssignment<Wrapped, true, false, kNoexceptMove> : public Wrapped {
protected:
    explicit ConditionalMoveAssignment(EmptyStorageTag tag) noexcept : Wrapped(tag) {}

public:
    using Wrapped::Wrapped;

    ConditionalMoveAssignment(const ConditionalMoveAssignment&) = default;
    ConditionalMoveAssignment(ConditionalMoveAssignment&&) = default;
    ConditionalMoveAssignment& operator=(const ConditionalMoveAssignment&) = default;

    ConditionalMoveAssignment& operator=(ConditionalMoveAssignment&& other) noexcept(
        kNoexceptMove) {
        if (this != std::addressof(other)) {
            this->move_assign_from(std::move(other));
        }
        return *this;
    }
};

template <class T, class E>
using NonVoidWithDestructor = ConditionalDestructor<
    ValueOrErrorStorage<T, E>,
    std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<E>>;

template <class T, class E>
using NonVoidWithCopyConstructor = ConditionalCopyConstructor<
    NonVoidWithDestructor<T, E>,
    std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>,
    std::is_trivially_copy_constructible_v<T> && std::is_trivially_copy_constructible_v<E>,
    std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_constructible_v<E>>;

template <class T, class E>
using NonVoidWithMoveConstructor = ConditionalMoveConstructor<
    NonVoidWithCopyConstructor<T, E>,
    std::is_move_constructible_v<T> && std::is_move_constructible_v<E>,
    std::is_trivially_move_constructible_v<T> && std::is_trivially_move_constructible_v<E>,
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>>;

template <class T, class E>
inline constexpr bool kNonVoidCopyAssignmentEnabled =
    std::is_copy_constructible_v<T>&& std::is_copy_assignable_v<T>&&
        std::is_copy_constructible_v<E>&& std::is_copy_assignable_v<E> &&
    (std::is_nothrow_copy_constructible_v<T> || std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_constructible_v<E>)&&(
        std::is_nothrow_copy_constructible_v<E> || std::is_nothrow_move_constructible_v<E> ||
        std::is_nothrow_move_constructible_v<T>);

template <class T, class E>
inline constexpr bool kNonVoidMoveAssignmentEnabled =
    std::is_move_constructible_v<T>&& std::is_move_assignable_v<T>&&
        std::is_move_constructible_v<E>&& std::is_move_assignable_v<E> &&
    (std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_constructible_v<E>);

template <class T, class E>
using NonVoidWithCopyAssignment = ConditionalCopyAssignment<
    NonVoidWithMoveConstructor<T, E>,
    kNonVoidCopyAssignmentEnabled<T, E>,
    kNonVoidCopyAssignmentEnabled<T, E> && std::is_trivially_copy_constructible_v<T> &&
        std::is_trivially_copy_assignable_v<T> && std::is_trivially_destructible_v<T> &&
        std::is_trivially_copy_constructible_v<E> && std::is_trivially_copy_assignable_v<E> &&
        std::is_trivially_destructible_v<E>,
    std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T> &&
        std::is_nothrow_copy_constructible_v<E> && std::is_nothrow_copy_assignable_v<E>>;

template <class T, class E>
using NonVoidSpecialMembers = ConditionalMoveAssignment<
    NonVoidWithCopyAssignment<T, E>,
    kNonVoidMoveAssignmentEnabled<T, E>,
    kNonVoidMoveAssignmentEnabled<T, E> && std::is_trivially_move_constructible_v<T> &&
        std::is_trivially_move_assignable_v<T> && std::is_trivially_destructible_v<T> &&
        std::is_trivially_move_constructible_v<E> && std::is_trivially_move_assignable_v<E> &&
        std::is_trivially_destructible_v<E>,
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
        std::is_nothrow_move_constructible_v<E> && std::is_nothrow_move_assignable_v<E>>;

template <class E>
using VoidWithDestructor =
    ConditionalDestructor<SuccessOrErrorStorage<E>, std::is_trivially_destructible_v<E>>;

template <class E>
using VoidWithCopyConstructor = ConditionalCopyConstructor<
    VoidWithDestructor<E>,
    std::is_copy_constructible_v<E>,
    std::is_trivially_copy_constructible_v<E>,
    std::is_nothrow_copy_constructible_v<E>>;

template <class E>
using VoidWithMoveConstructor = ConditionalMoveConstructor<
    VoidWithCopyConstructor<E>,
    std::is_move_constructible_v<E>,
    std::is_trivially_move_constructible_v<E>,
    std::is_nothrow_move_constructible_v<E>>;

template <class E>
inline constexpr bool kVoidCopyAssignmentEnabled =
    std::is_copy_constructible_v<E>&& std::is_copy_assignable_v<E>;

template <class E>
inline constexpr bool kVoidMoveAssignmentEnabled =
    std::is_move_constructible_v<E>&& std::is_move_assignable_v<E>;

template <class E>
using VoidWithCopyAssignment = ConditionalCopyAssignment<
    VoidWithMoveConstructor<E>,
    kVoidCopyAssignmentEnabled<E>,
    kVoidCopyAssignmentEnabled<E> && std::is_trivially_copy_constructible_v<E> &&
        std::is_trivially_copy_assignable_v<E> && std::is_trivially_destructible_v<E>,
    std::is_nothrow_copy_constructible_v<E> && std::is_nothrow_copy_assignable_v<E>>;

template <class E>
using VoidSpecialMembers = ConditionalMoveAssignment<
    VoidWithCopyAssignment<E>,
    kVoidMoveAssignmentEnabled<E>,
    kVoidMoveAssignmentEnabled<E> && std::is_trivially_move_constructible_v<E> &&
        std::is_trivially_move_assignable_v<E> && std::is_trivially_destructible_v<E>,
    std::is_nothrow_move_constructible_v<E> && std::is_nothrow_move_assignable_v<E>>;

}  // namespace result_special_members

namespace result_traits {

template <class T, class E, class U>
inline constexpr bool kValueConstructorEnabled =
    std::is_constructible_v<T, U&&> && !std::is_same_v<RemoveCvRefT<U>, Result<T, E>> &&
    !std::is_same_v<RemoveCvRefT<U>, std::in_place_t> &&
    !std::is_same_v<RemoveCvRefT<U>, unexpect_t> && !kIsUnexpected<U> &&
    !(std::is_same_v<T, bool> && kIsResult<U>);

template <class T, class E, class U>
inline constexpr bool kValueAssignmentEnabled =
    !std::is_same_v<RemoveCvRefT<U>, Result<T, E>> && !kIsUnexpected<U> &&
    std::is_constructible_v<T, U&&> && std::is_assignable_v<T&, U&&> &&
    (std::is_nothrow_constructible_v<T, U&&> || std::is_nothrow_move_constructible_v<T> ||
     std::is_nothrow_move_constructible_v<E>);

template <class New, class Old, class... Args>
inline constexpr bool kCanReinitialize =
    std::is_nothrow_constructible_v<New, Args...> || std::is_nothrow_move_constructible_v<New> ||
    std::is_nothrow_move_constructible_v<Old>;

}  // namespace result_traits

template <class T>
inline constexpr bool is_result_v = result_traits::kIsResult<T>;

template <class E>
inline constexpr bool is_result_error_v = result_traits::IsValidErrorType<E>::value;

template <class E>
class Unexpected {
    static_assert(
        is_result_error_v<E>,
        "Unexpected<E> requires a complete, "
        "non-abstract, unqualified, "
        "nothrow-destructible object type");

public:
    using error_type = E;

    template <class U = E, std::enable_if_t<std::is_copy_constructible_v<U>, int> = 0>
    constexpr explicit Unexpected(const E& error) noexcept(std::is_nothrow_copy_constructible_v<E>)
    : error_(error) {}

    template <class U = E, std::enable_if_t<std::is_move_constructible_v<U>, int> = 0>
    constexpr explicit Unexpected(E&& error) noexcept(std::is_nothrow_move_constructible_v<E>)
    : error_(std::move(error)) {}

    template <class... Args, std::enable_if_t<std::is_constructible_v<E, Args&&...>, int> = 0>
    constexpr explicit Unexpected(std::in_place_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : error_(std::forward<Args>(args)...) {}

    constexpr E& error() & noexcept { return error_; }

    constexpr const E& error() const& noexcept { return error_; }

    constexpr E&& error() && noexcept { return std::move(error_); }

    constexpr const E&& error() const&& noexcept { return std::move(error_); }

private:
    E error_;
};

template <class E>
constexpr auto make_unexpected(E&& error) noexcept(
    std::is_nothrow_constructible_v<Unexpected<std::decay_t<E>>, E&&>)
    -> Unexpected<std::decay_t<E>> {
    return Unexpected<std::decay_t<E>>(std::forward<E>(error));
}

template <class T, class E>
class [[nodiscard]] Result : private result_special_members::NonVoidSpecialMembers<T, E> {
    static_assert(
        result_traits::IsValidValueType<T>::value,
        "Result<T,E> requires T to be a "
        "complete, non-abstract, unqualified, "
        "nothrow-destructible object type");
    static_assert(
        is_result_error_v<E>,
        "Result<T,E> requires E to be a "
        "complete, non-abstract, unqualified, "
        "nothrow-destructible object type");

    using SpecialMembers = result_special_members::NonVoidSpecialMembers<T, E>;

public:
    using value_type = T;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    template <class U>
    using rebind = Result<U, E>;

    template <class U = T, std::enable_if_t<std::is_default_constructible_v<U>, int> = 0>
    constexpr Result() noexcept(std::is_nothrow_default_constructible_v<T>)
    : SpecialMembers(std::in_place) {}

    template <
        class U,
        std::enable_if_t<
            result_traits::kValueConstructorEnabled<T, E, U> && std::is_convertible_v<U&&, T>,
            int> = 0>
    constexpr Result(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
    : SpecialMembers(std::in_place, std::forward<U>(value)) {}

    template <
        class U,
        std::enable_if_t<
            result_traits::kValueConstructorEnabled<T, E, U> && !std::is_convertible_v<U&&, T>,
            int> = 0>
    explicit constexpr Result(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
    : SpecialMembers(std::in_place, std::forward<U>(value)) {}

    template <class... Args, std::enable_if_t<std::is_constructible_v<T, Args&&...>, int> = 0>
    explicit constexpr Result(std::in_place_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>)
    : SpecialMembers(std::in_place, std::forward<Args>(args)...) {}

    template <class... Args, std::enable_if_t<std::is_constructible_v<E, Args&&...>, int> = 0>
    explicit constexpr Result(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : SpecialMembers(unexpect, std::forward<Args>(args)...) {}

    template <
        class G,
        std::enable_if_t<
            std::is_constructible_v<E, const G&> && std::is_convertible_v<const G&, E>,
            int> = 0>
    constexpr Result(const Unexpected<G>& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, const G&>)
    : SpecialMembers(unexpect, unexpected.error()) {}

    template <
        class G,
        std::enable_if_t<
            std::is_constructible_v<E, const G&> && !std::is_convertible_v<const G&, E>,
            int> = 0>
    explicit constexpr Result(const Unexpected<G>& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, const G&>)
    : SpecialMembers(unexpect, unexpected.error()) {}

    template <
        class G,
        std::enable_if_t<std::is_constructible_v<E, G&&> && std::is_convertible_v<G&&, E>, int> = 0>
    constexpr Result(Unexpected<G>&& unexpected) noexcept(std::is_nothrow_constructible_v<E, G&&>)
    : SpecialMembers(unexpect, std::move(unexpected).error()) {}

    template <
        class G,
        std::enable_if_t<std::is_constructible_v<E, G&&> && !std::is_convertible_v<G&&, E>, int> =
            0>
    explicit constexpr Result(Unexpected<G>&& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, G&&>)
    : SpecialMembers(unexpect, std::move(unexpected).error()) {}

    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;
    ~Result() = default;

    template <class U, std::enable_if_t<result_traits::kValueAssignmentEnabled<T, E, U>, int> = 0>
    Result& operator=(U&& value) noexcept(
        std::is_nothrow_assignable_v<T&, U&&>&& std::is_nothrow_constructible_v<T, U&&>) {
        if (has_value()) {
            this->value_ref() = std::forward<U>(value);
        } else {
            this->reinit_value(std::forward<U>(value));
        }
        return *this;
    }

    template <
        class G,
        std::enable_if_t<
            std::is_constructible_v<E, const G&> && std::is_assignable_v<E&, const G&> &&
                result_traits::kCanReinitialize<E, T, const G&>,
            int> = 0>
    Result& operator=(const Unexpected<G>& unexpected) noexcept(
        std::is_nothrow_assignable_v<E&, const G&>&& std::is_nothrow_constructible_v<E, const G&>) {
        if (has_value()) {
            this->reinit_error(unexpected.error());
        } else {
            this->error_ref() = unexpected.error();
        }
        return *this;
    }

    template <
        class G,
        std::enable_if_t<
            std::is_constructible_v<E, G&&> && std::is_assignable_v<E&, G&&> &&
                result_traits::kCanReinitialize<E, T, G&&>,
            int> = 0>
    Result& operator=(Unexpected<G>&& unexpected) noexcept(
        std::is_nothrow_assignable_v<E&, G&&>&& std::is_nothrow_constructible_v<E, G&&>) {
        if (has_value()) {
            this->reinit_error(std::move(unexpected).error());
        } else {
            this->error_ref() = std::move(unexpected).error();
        }
        return *this;
    }

    constexpr bool has_value() const noexcept { return this->stores_value(); }

    constexpr explicit operator bool() const noexcept { return has_value(); }

    T& value() & noexcept {
        if (!has_value()) {
            terminate_bad_access();
        }
        return this->value_ref();
    }

    const T& value() const& noexcept {
        if (!has_value()) {
            terminate_bad_access();
        }
        return this->value_ref();
    }

    T&& value() && noexcept {
        if (!has_value()) {
            terminate_bad_access();
        }
        return std::move(*this).value_ref();
    }

    const T&& value() const&& noexcept {
        if (!has_value()) {
            terminate_bad_access();
        }
        return std::move(*this).value_ref();
    }

    T& operator*() & noexcept { return value(); }

    const T& operator*() const& noexcept { return value(); }

    T&& operator*() && noexcept { return std::move(*this).value(); }

    const T&& operator*() const&& noexcept { return std::move(*this).value(); }

    T* operator->() noexcept { return std::addressof(value()); }

    const T* operator->() const noexcept { return std::addressof(value()); }

    E& error() & noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return this->error_ref();
    }

    const E& error() const& noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return this->error_ref();
    }

    E&& error() && noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return std::move(*this).error_ref();
    }

    const E&& error() const&& noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return std::move(*this).error_ref();
    }

    template <
        class U,
        std::enable_if_t<std::is_copy_constructible_v<T> && std::is_convertible_v<U&&, T>, int> = 0>
    T value_or(U&& default_value) const& {
        if (has_value()) {
            return this->value_ref();
        }
        return static_cast<T>(std::forward<U>(default_value));
    }

    template <
        class U,
        std::enable_if_t<std::is_move_constructible_v<T> && std::is_convertible_v<U&&, T>, int> = 0>
    T value_or(U&& default_value) && {
        if (has_value()) {
            return std::move(*this).value_ref();
        }
        return static_cast<T>(std::forward<U>(default_value));
    }

    template <
        class... Args,
        std::enable_if_t<std::is_nothrow_constructible_v<T, Args&&...>, int> = 0>
    T& emplace(Args&&... args) noexcept {
        return this->emplace_value(std::forward<Args>(args)...);
    }

    template <class F>
    auto and_then(F&& function) & {
        return and_then_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto and_then(F&& function) const& {
        return and_then_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto and_then(F&& function) && {
        return and_then_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto and_then(F&& function) const&& {
        return and_then_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) & {
        return transform_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) const& {
        return transform_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) && {
        return transform_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) const&& {
        return transform_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) & {
        return or_else_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) const& {
        return or_else_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) && {
        return or_else_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) const&& {
        return or_else_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) & {
        return transform_error_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) const& {
        return transform_error_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) && {
        return transform_error_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) const&& {
        return transform_error_impl(std::move(*this), std::forward<F>(function));
    }

private:
    [[noreturn]] static void terminate_bad_access() noexcept { std::terminate(); }

    template <class Self, class F>
    static auto and_then_impl(Self&& self, F&& function) {
        using ValueArg = decltype(std::forward<Self>(self).value());

        static_assert(
            std::is_invocable_v<F, ValueArg>,
            "Result::and_then requires an "
            "invocable callable");

        if constexpr (std::is_invocable_v<F, ValueArg>) {
            using Raw = std::invoke_result_t<F, ValueArg>;
            using Return = result_traits::RemoveCvRefT<Raw>;

            static_assert(
                is_result_v<Return>,
                "Result::and_then requires F "
                "to return Result<U,E>");

            if constexpr (is_result_v<Return>) {
                static_assert(
                    std::is_same_v<typename Return::error_type, E>,
                    "Result::and_then cannot "
                    "change error_type");
                static_assert(
                    std::is_constructible_v<Return, Raw>,
                    "Result::and_then return "
                    "expression cannot construct "
                    "the normalized Result");

                using ErrorArg = decltype(std::forward<Self>(self).error());

                static_assert(
                    std::is_constructible_v<E, ErrorArg>,
                    "Result::and_then requires "
                    "error propagation for this "
                    "cv-ref overload");

                if constexpr (
                    std::is_same_v<typename Return::error_type, E> &&
                    std::is_constructible_v<Return, Raw> && std::is_constructible_v<E, ErrorArg>) {
                    if (self.has_value()) {
                        return static_cast<Return>(std::invoke(
                            std::forward<F>(function),
                            std::forward<Self>(self).value()));
                    }

                    return Return(unexpect, std::forward<Self>(self).error());
                }
            }
        }
    }

    template <class Self, class F>
    static auto transform_impl(Self&& self, F&& function) {
        using ValueArg = decltype(std::forward<Self>(self).value());

        static_assert(
            std::is_invocable_v<F, ValueArg>,
            "Result::transform requires an "
            "invocable callable");

        if constexpr (std::is_invocable_v<F, ValueArg>) {
            using Raw = std::invoke_result_t<F, ValueArg>;
            using U = std::remove_cv_t<Raw>;

            constexpr bool kValidValue =
                std::is_void_v<U> || result_traits::IsValidValueType<U>::value;

            static_assert(
                kValidValue,
                "Result::transform requires a "
                "non-reference valid value type "
                "or void");

            constexpr bool kTargetConstructible =
                std::is_void_v<U> || std::is_constructible_v<U, Raw>;

            static_assert(
                kTargetConstructible,
                "Result::transform target value "
                "must be constructible from F's "
                "return type");

            using ErrorArg = decltype(std::forward<Self>(self).error());

            static_assert(
                std::is_constructible_v<E, ErrorArg>,
                "Result::transform requires "
                "error propagation for this "
                "cv-ref overload");

            if constexpr (
                kValidValue && kTargetConstructible && std::is_constructible_v<E, ErrorArg>) {
                using Return = Result<U, E>;

                if (self.has_value()) {
                    if constexpr (std::is_void_v<U>) {
                        std::invoke(std::forward<F>(function), std::forward<Self>(self).value());
                        return Return{};
                    } else {
                        return Return(
                            std::in_place,
                            std::invoke(
                                std::forward<F>(function),
                                std::forward<Self>(self).value()));
                    }
                }

                return Return(unexpect, std::forward<Self>(self).error());
            }
        }
    }

    template <class Self, class F>
    static auto or_else_impl(Self&& self, F&& function) {
        using ErrorArg = decltype(std::forward<Self>(self).error());

        static_assert(
            std::is_invocable_v<F, ErrorArg>,
            "Result::or_else requires an "
            "invocable callable");

        if constexpr (std::is_invocable_v<F, ErrorArg>) {
            using Raw = std::invoke_result_t<F, ErrorArg>;
            using Return = result_traits::RemoveCvRefT<Raw>;

            static_assert(
                is_result_v<Return>,
                "Result::or_else requires F "
                "to return Result<T,G>");

            if constexpr (is_result_v<Return>) {
                static_assert(
                    std::is_same_v<typename Return::value_type, T>,
                    "Result::or_else cannot "
                    "change value_type");
                static_assert(
                    std::is_constructible_v<Return, Raw>,
                    "Result::or_else return "
                    "expression cannot construct "
                    "the normalized Result");

                using ValueArg = decltype(std::forward<Self>(self).value());

                static_assert(
                    std::is_constructible_v<T, ValueArg>,
                    "Result::or_else requires "
                    "value propagation for this "
                    "cv-ref overload");

                if constexpr (
                    std::is_same_v<typename Return::value_type, T> &&
                    std::is_constructible_v<Return, Raw> && std::is_constructible_v<T, ValueArg>) {
                    if (self.has_value()) {
                        return Return(std::in_place, std::forward<Self>(self).value());
                    }

                    return static_cast<Return>(
                        std::invoke(std::forward<F>(function), std::forward<Self>(self).error()));
                }
            }
        }
    }

    template <class Self, class F>
    static auto transform_error_impl(Self&& self, F&& function) {
        using ErrorArg = decltype(std::forward<Self>(self).error());

        static_assert(
            std::is_invocable_v<F, ErrorArg>,
            "Result::transform_error requires "
            "an invocable callable");

        if constexpr (std::is_invocable_v<F, ErrorArg>) {
            using Raw = std::invoke_result_t<F, ErrorArg>;
            using G = std::remove_cv_t<Raw>;

            static_assert(
                is_result_error_v<G>,
                "Result::transform_error "
                "requires a valid non-reference "
                "error type");

            constexpr bool kTargetConstructible = std::is_constructible_v<G, Raw>;

            static_assert(
                kTargetConstructible,
                "Result::transform_error target "
                "error must be constructible from "
                "F's return type");

            using ValueArg = decltype(std::forward<Self>(self).value());

            static_assert(
                std::is_constructible_v<T, ValueArg>,
                "Result::transform_error "
                "requires value propagation "
                "for this cv-ref overload");

            if constexpr (
                is_result_error_v<G> && kTargetConstructible &&
                std::is_constructible_v<T, ValueArg>) {
                using Return = Result<T, G>;

                if (self.has_value()) {
                    return Return(std::in_place, std::forward<Self>(self).value());
                }

                return Return(
                    unexpect,
                    std::invoke(std::forward<F>(function), std::forward<Self>(self).error()));
            }
        }
    }
};

template <class E>
class [[nodiscard]] Result<void, E> : private result_special_members::VoidSpecialMembers<E> {
    static_assert(
        is_result_error_v<E>,
        "Result<void,E> requires E to be a "
        "complete, non-abstract, unqualified, "
        "nothrow-destructible object type");

    using SpecialMembers = result_special_members::VoidSpecialMembers<E>;

public:
    using value_type = void;
    using error_type = E;
    using unexpected_type = Unexpected<E>;

    template <class U>
    using rebind = Result<U, E>;

    constexpr Result() noexcept : SpecialMembers(std::in_place) {}

    explicit constexpr Result(std::in_place_t) noexcept : SpecialMembers(std::in_place) {}

    template <class... Args, std::enable_if_t<std::is_constructible_v<E, Args&&...>, int> = 0>
    explicit constexpr Result(unexpect_t, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<E, Args&&...>)
    : SpecialMembers(unexpect, std::forward<Args>(args)...) {}

    template <
        class G,
        std::enable_if_t<
            std::is_constructible_v<E, const G&> && std::is_convertible_v<const G&, E>,
            int> = 0>
    constexpr Result(const Unexpected<G>& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, const G&>)
    : SpecialMembers(unexpect, unexpected.error()) {}

    template <
        class G,
        std::enable_if_t<
            std::is_constructible_v<E, const G&> && !std::is_convertible_v<const G&, E>,
            int> = 0>
    explicit constexpr Result(const Unexpected<G>& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, const G&>)
    : SpecialMembers(unexpect, unexpected.error()) {}

    template <
        class G,
        std::enable_if_t<std::is_constructible_v<E, G&&> && std::is_convertible_v<G&&, E>, int> = 0>
    constexpr Result(Unexpected<G>&& unexpected) noexcept(std::is_nothrow_constructible_v<E, G&&>)
    : SpecialMembers(unexpect, std::move(unexpected).error()) {}

    template <
        class G,
        std::enable_if_t<std::is_constructible_v<E, G&&> && !std::is_convertible_v<G&&, E>, int> =
            0>
    explicit constexpr Result(Unexpected<G>&& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, G&&>)
    : SpecialMembers(unexpect, std::move(unexpected).error()) {}

    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;
    ~Result() = default;

    template <
        class G,
        std::enable_if_t<
            std::is_constructible_v<E, const G&> && std::is_assignable_v<E&, const G&>,
            int> = 0>
    Result& operator=(const Unexpected<G>& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, const G&>&& std::is_nothrow_assignable_v<E&, const G&>) {
        if (has_value()) {
            this->reinit_error_from_success(unexpected.error());
        } else {
            this->error_ref() = unexpected.error();
        }
        return *this;
    }

    template <
        class G,
        std::enable_if_t<std::is_constructible_v<E, G&&> && std::is_assignable_v<E&, G&&>, int> = 0>
    Result& operator=(Unexpected<G>&& unexpected) noexcept(
        std::is_nothrow_constructible_v<E, G&&>&& std::is_nothrow_assignable_v<E&, G&&>) {
        if (has_value()) {
            this->reinit_error_from_success(std::move(unexpected).error());
        } else {
            this->error_ref() = std::move(unexpected).error();
        }
        return *this;
    }

    constexpr bool has_value() const noexcept { return this->stores_value(); }

    constexpr explicit operator bool() const noexcept { return has_value(); }

    void value() const noexcept {
        if (!has_value()) {
            terminate_bad_access();
        }
    }

    void operator*() const noexcept { value(); }

    E& error() & noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return this->error_ref();
    }

    const E& error() const& noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return this->error_ref();
    }

    E&& error() && noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return std::move(*this).error_ref();
    }

    const E&& error() const&& noexcept {
        if (has_value()) {
            terminate_bad_access();
        }
        return std::move(*this).error_ref();
    }

    void emplace() noexcept { this->emplace_success(); }

    template <class F>
    auto and_then(F&& function) & {
        return and_then_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto and_then(F&& function) const& {
        return and_then_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto and_then(F&& function) && {
        return and_then_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto and_then(F&& function) const&& {
        return and_then_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) & {
        return transform_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) const& {
        return transform_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) && {
        return transform_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform(F&& function) const&& {
        return transform_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) & {
        return or_else_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) const& {
        return or_else_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) && {
        return or_else_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto or_else(F&& function) const&& {
        return or_else_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) & {
        return transform_error_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) const& {
        return transform_error_impl(*this, std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) && {
        return transform_error_impl(std::move(*this), std::forward<F>(function));
    }

    template <class F>
    auto transform_error(F&& function) const&& {
        return transform_error_impl(std::move(*this), std::forward<F>(function));
    }

private:
    [[noreturn]] static void terminate_bad_access() noexcept { std::terminate(); }

    template <class Self, class F>
    static auto and_then_impl(Self&& self, F&& function) {
        static_assert(
            std::is_invocable_v<F>,
            "Result<void,E>::and_then "
            "requires an invocable callable");

        if constexpr (std::is_invocable_v<F>) {
            using Raw = std::invoke_result_t<F>;
            using Return = result_traits::RemoveCvRefT<Raw>;

            static_assert(
                is_result_v<Return>,
                "Result<void,E>::and_then "
                "requires F to return "
                "Result<U,E>");

            if constexpr (is_result_v<Return>) {
                static_assert(
                    std::is_same_v<typename Return::error_type, E>,
                    "Result<void,E>::and_then "
                    "cannot change error_type");
                static_assert(
                    std::is_constructible_v<Return, Raw>,
                    "Result<void,E>::and_then "
                    "return expression cannot "
                    "construct normalized Result");

                using ErrorArg = decltype(std::forward<Self>(self).error());

                static_assert(
                    std::is_constructible_v<E, ErrorArg>,
                    "Result<void,E>::and_then "
                    "requires error propagation");

                if constexpr (
                    std::is_same_v<typename Return::error_type, E> &&
                    std::is_constructible_v<Return, Raw> && std::is_constructible_v<E, ErrorArg>) {
                    if (self.has_value()) {
                        return static_cast<Return>(std::invoke(std::forward<F>(function)));
                    }

                    return Return(unexpect, std::forward<Self>(self).error());
                }
            }
        }
    }

    template <class Self, class F>
    static auto transform_impl(Self&& self, F&& function) {
        static_assert(
            std::is_invocable_v<F>,
            "Result<void,E>::transform "
            "requires an invocable callable");

        if constexpr (std::is_invocable_v<F>) {
            using Raw = std::invoke_result_t<F>;
            using U = std::remove_cv_t<Raw>;

            constexpr bool kValidValue =
                std::is_void_v<U> || result_traits::IsValidValueType<U>::value;

            static_assert(
                kValidValue,
                "Result<void,E>::transform "
                "requires a valid "
                "non-reference value type "
                "or void");

            constexpr bool kTargetConstructible =
                std::is_void_v<U> || std::is_constructible_v<U, Raw>;

            static_assert(
                kTargetConstructible,
                "Result<void,E>::transform target "
                "value must be constructible from "
                "F's return type");

            using ErrorArg = decltype(std::forward<Self>(self).error());

            static_assert(
                std::is_constructible_v<E, ErrorArg>,
                "Result<void,E>::transform "
                "requires error propagation");

            if constexpr (
                kValidValue && kTargetConstructible && std::is_constructible_v<E, ErrorArg>) {
                using Return = Result<U, E>;

                if (self.has_value()) {
                    if constexpr (std::is_void_v<U>) {
                        std::invoke(std::forward<F>(function));
                        return Return{};
                    } else {
                        return Return(std::in_place, std::invoke(std::forward<F>(function)));
                    }
                }

                return Return(unexpect, std::forward<Self>(self).error());
            }
        }
    }

    template <class Self, class F>
    static auto or_else_impl(Self&& self, F&& function) {
        using ErrorArg = decltype(std::forward<Self>(self).error());

        static_assert(
            std::is_invocable_v<F, ErrorArg>,
            "Result<void,E>::or_else "
            "requires an invocable callable");

        if constexpr (std::is_invocable_v<F, ErrorArg>) {
            using Raw = std::invoke_result_t<F, ErrorArg>;
            using Return = result_traits::RemoveCvRefT<Raw>;

            static_assert(
                is_result_v<Return>,
                "Result<void,E>::or_else "
                "requires F to return "
                "Result<void,G>");

            if constexpr (is_result_v<Return>) {
                static_assert(
                    std::is_void_v<typename Return::value_type>,
                    "Result<void,E>::or_else "
                    "cannot change value_type");
                static_assert(
                    std::is_constructible_v<Return, Raw>,
                    "Result<void,E>::or_else "
                    "return expression cannot "
                    "construct normalized Result");

                if constexpr (
                    std::is_void_v<typename Return::value_type> &&
                    std::is_constructible_v<Return, Raw>) {
                    if (self.has_value()) {
                        return Return{};
                    }

                    return static_cast<Return>(
                        std::invoke(std::forward<F>(function), std::forward<Self>(self).error()));
                }
            }
        }
    }

    template <class Self, class F>
    static auto transform_error_impl(Self&& self, F&& function) {
        using ErrorArg = decltype(std::forward<Self>(self).error());

        static_assert(
            std::is_invocable_v<F, ErrorArg>,
            "Result<void,E>::transform_error "
            "requires an invocable callable");

        if constexpr (std::is_invocable_v<F, ErrorArg>) {
            using Raw = std::invoke_result_t<F, ErrorArg>;
            using G = std::remove_cv_t<Raw>;

            static_assert(
                is_result_error_v<G>,
                "Result<void,E>::transform_error "
                "requires a valid "
                "non-reference error type");

            constexpr bool kTargetConstructible = std::is_constructible_v<G, Raw>;

            static_assert(
                kTargetConstructible,
                "Result<void,E>::transform_error "
                "target error must be "
                "constructible from F's return "
                "type");

            if constexpr (is_result_error_v<G> && kTargetConstructible) {
                using Return = Result<void, G>;

                if (self.has_value()) {
                    return Return{};
                }

                return Return(
                    unexpect,
                    std::invoke(std::forward<F>(function), std::forward<Self>(self).error()));
            }
        }
    }
};

}  // namespace ros2_bag

#endif  // ROS2_BAG_RESULT_HPP_
