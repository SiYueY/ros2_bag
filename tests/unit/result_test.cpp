#include "ros2_bag/result.hpp"

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

enum class ErrorCode : int { One, Two };

struct TestFailure {};

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }

    ++g_failures;
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <class Function>
bool terminates(Function&& function) {
    const pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "fork failed: " << errno << '\n';
        return false;
    }

    if (child == 0) {
        std::set_terminate([] { ::_exit(91); });

        function();
        ::_exit(0);
    }

    int status = 0;
    if (::waitpid(child, &status, 0) != child) {
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 91;
}

struct Incomplete;

struct AbstractType {
    virtual ~AbstractType() noexcept = default;
    virtual void run() = 0;
};

struct ErrorNoDefault {
    ErrorNoDefault() = delete;
    explicit ErrorNoDefault(int value) noexcept : value(value) {}

    int value = 0;
};

struct FromUnexpect {
    explicit FromUnexpect(ros2_bag::unexpect_t) noexcept : value(17) {}

    FromUnexpect& operator=(ros2_bag::unexpect_t) noexcept {
        value = 18;
        return *this;
    }

    int value = 0;
};

struct TagValue {
    explicit TagValue(std::in_place_t) noexcept : value(1) {}

    explicit TagValue(ros2_bag::unexpect_t) noexcept : value(2) {}

    TagValue& operator=(std::in_place_t) noexcept {
        value = 3;
        return *this;
    }

    TagValue& operator=(ros2_bag::unexpect_t) noexcept {
        value = 4;
        return *this;
    }

    int value = 0;
};

struct ImplicitValue {
    ImplicitValue(int value) noexcept : value(value) {}

    int value = 0;
};

struct ExplicitValue {
    explicit ExplicitValue(int value) noexcept : value(value) {}

    int value = 0;
};

struct ImplicitError {
    ImplicitError(int value) noexcept : value(value) {}

    int value = 0;
};

struct ExplicitError {
    explicit ExplicitError(int value) noexcept : value(value) {}

    int value = 0;
};

struct DirectWrapper {
    template <class U>
    explicit DirectWrapper(U&&) noexcept {}
};

struct MoveOnlyTrivial {
    int value = 0;

    MoveOnlyTrivial() = default;
    MoveOnlyTrivial(const MoveOnlyTrivial&) = delete;
    MoveOnlyTrivial& operator=(const MoveOnlyTrivial&) = delete;
    MoveOnlyTrivial(MoveOnlyTrivial&&) = default;
    MoveOnlyTrivial& operator=(MoveOnlyTrivial&&) = default;
    ~MoveOnlyTrivial() = default;
};

struct CopyOnlyTrivial {
    int value = 0;

    CopyOnlyTrivial() = default;
    CopyOnlyTrivial(const CopyOnlyTrivial&) = default;
    CopyOnlyTrivial& operator=(const CopyOnlyTrivial&) = default;
    CopyOnlyTrivial(CopyOnlyTrivial&&) = delete;
    CopyOnlyTrivial& operator=(CopyOnlyTrivial&&) = delete;
    ~CopyOnlyTrivial() = default;
};

struct ThrowingMove {
    int value = 0;

    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) = default;
    ThrowingMove& operator=(const ThrowingMove&) = default;

    ThrowingMove(ThrowingMove&& other) noexcept(false) : value(other.value) {}

    ThrowingMove& operator=(ThrowingMove&& other) noexcept(false) {
        value = other.value;
        return *this;
    }

    ~ThrowingMove() noexcept = default;
};

struct Lifetime {
    static inline int live_count = 0;
    static inline int copy_count = 0;
    static inline int move_count = 0;

    static void reset() {
        live_count = 0;
        copy_count = 0;
        move_count = 0;
    }

    explicit Lifetime(int value = 0) noexcept : value(value) { ++live_count; }

    Lifetime(const Lifetime& other) noexcept : value(other.value) {
        ++live_count;
        ++copy_count;
    }

    Lifetime(Lifetime&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live_count;
        ++move_count;
    }

    Lifetime& operator=(const Lifetime& other) noexcept {
        value = other.value;
        ++copy_count;
        return *this;
    }

    Lifetime& operator=(Lifetime&& other) noexcept {
        value = other.value;
        other.value = -1;
        ++move_count;
        return *this;
    }

    ~Lifetime() noexcept { --live_count; }

    int value = 0;
};

struct ConversionInput {
    int value = 0;
    bool throw_now = false;
};

struct BackupError {
    static inline int move_count = 0;

    static void reset() { move_count = 0; }

    explicit BackupError(int value = 0) noexcept : value(value) {}

    BackupError(const BackupError&) = default;

    BackupError(BackupError&& other) noexcept : value(other.value) {
        other.value = -1;
        ++move_count;
    }

    BackupError& operator=(const BackupError&) = default;

    BackupError& operator=(BackupError&& other) noexcept {
        value = other.value;
        other.value = -1;
        ++move_count;
        return *this;
    }

    int value = 0;
};

struct BackupValue {
    static inline int move_count = 0;

    static void reset() { move_count = 0; }

    explicit BackupValue(int value = 0) noexcept : value(value) {}

    BackupValue(const BackupValue&) = default;

    BackupValue(BackupValue&& other) noexcept : value(other.value) {
        other.value = -1;
        ++move_count;
    }

    BackupValue& operator=(const BackupValue&) = default;

    BackupValue& operator=(BackupValue&& other) noexcept {
        value = other.value;
        other.value = -1;
        ++move_count;
        return *this;
    }

    int value = 0;
};

struct StrategyAValue {
    static inline int conversion_count = 0;
    static inline int move_count = 0;

    static void reset() {
        conversion_count = 0;
        move_count = 0;
    }

    explicit StrategyAValue(ConversionInput input) noexcept : value(input.value) {
        ++conversion_count;
    }

    StrategyAValue(const StrategyAValue&) = default;

    StrategyAValue(StrategyAValue&& other) noexcept(false) : value(other.value) {
        other.value = -1;
        ++move_count;
    }

    StrategyAValue& operator=(ConversionInput input) noexcept {
        value = input.value;
        return *this;
    }

    StrategyAValue& operator=(const StrategyAValue&) = default;

    StrategyAValue& operator=(StrategyAValue&&) = default;

    int value = 0;
};

struct StrategyBValue {
    static inline int conversion_count = 0;
    static inline int move_count = 0;

    static void reset() {
        conversion_count = 0;
        move_count = 0;
    }

    explicit StrategyBValue(ConversionInput input) {
        ++conversion_count;
        if (input.throw_now) {
            throw TestFailure{};
        }
        value = input.value;
    }

    StrategyBValue(const StrategyBValue&) = default;

    StrategyBValue(StrategyBValue&& other) noexcept : value(other.value) {
        other.value = -1;
        ++move_count;
    }

    StrategyBValue& operator=(ConversionInput input) {
        if (input.throw_now) {
            throw TestFailure{};
        }
        value = input.value;
        return *this;
    }

    StrategyBValue& operator=(const StrategyBValue&) = default;

    StrategyBValue& operator=(StrategyBValue&&) noexcept = default;

    int value = 0;
};

struct StrategyCValue {
    static inline int conversion_count = 0;
    static inline int move_count = 0;

    static void reset() {
        conversion_count = 0;
        move_count = 0;
    }

    explicit StrategyCValue(ConversionInput input) {
        ++conversion_count;
        if (input.throw_now) {
            throw TestFailure{};
        }
        value = input.value;
    }

    StrategyCValue(const StrategyCValue&) = default;

    StrategyCValue(StrategyCValue&& other) noexcept(false) : value(other.value) {
        other.value = -1;
        ++move_count;
    }

    StrategyCValue& operator=(ConversionInput input) {
        if (input.throw_now) {
            throw TestFailure{};
        }
        value = input.value;
        return *this;
    }

    StrategyCValue& operator=(const StrategyCValue&) = default;

    StrategyCValue& operator=(StrategyCValue&&) = default;

    int value = 0;
};

struct ThrowingAssign {
    explicit ThrowingAssign(int value = 0) noexcept : value(value) {}

    ThrowingAssign(const ThrowingAssign&) = default;

    ThrowingAssign(ThrowingAssign&&) noexcept = default;

    ThrowingAssign& operator=(const ThrowingAssign&) { throw TestFailure{}; }

    ThrowingAssign& operator=(ThrowingAssign&&) { throw TestFailure{}; }

    ~ThrowingAssign() noexcept = default;

    int value = 0;
};

struct Box {
    int value = 0;
};

struct ConstBoxCallable {
    const Box operator()(int value) const { return Box{value + 1}; }
};

struct MoveOnlyError {
    explicit MoveOnlyError(int value = 0) noexcept : value(value) {}

    MoveOnlyError(const MoveOnlyError&) = delete;
    MoveOnlyError& operator=(const MoveOnlyError&) = delete;

    MoveOnlyError(MoveOnlyError&&) noexcept = default;
    MoveOnlyError& operator=(MoveOnlyError&&) noexcept = default;

    int value = 0;
};

struct ThrowingCopyValue {
    static inline bool throw_on_copy = false;

    explicit ThrowingCopyValue(int value = 0) noexcept : value(value) {}

    ThrowingCopyValue(const ThrowingCopyValue& other) {
        if (throw_on_copy) {
            throw TestFailure{};
        }
        value = other.value;
    }

    ThrowingCopyValue(ThrowingCopyValue&& other) noexcept : value(other.value) { other.value = -1; }

    ThrowingCopyValue& operator=(const ThrowingCopyValue& other) {
        if (throw_on_copy) {
            throw TestFailure{};
        }
        value = other.value;
        return *this;
    }

    ThrowingCopyValue& operator=(ThrowingCopyValue&& other) noexcept {
        value = other.value;
        other.value = -1;
        return *this;
    }

    ~ThrowingCopyValue() noexcept = default;

    int value = 0;
};

struct ThrowingMoveError {
    static inline bool throw_on_move = false;

    explicit ThrowingMoveError(int value = 0) noexcept : value(value) {}

    ThrowingMoveError(const ThrowingMoveError&) = default;

    ThrowingMoveError(ThrowingMoveError&& other) {
        value = other.value;
        other.value = -1;
        if (throw_on_move) {
            throw TestFailure{};
        }
    }

    ThrowingMoveError& operator=(const ThrowingMoveError&) = default;

    ThrowingMoveError& operator=(ThrowingMoveError&& other) {
        value = other.value;
        other.value = -1;
        if (throw_on_move) {
            throw TestFailure{};
        }
        return *this;
    }

    ~ThrowingMoveError() noexcept = default;

    int value = 0;
};

template <class R, class U, class = void>
struct HasConstValueOr : std::false_type {};

template <class R, class U>
struct HasConstValueOr<
    R, U, std::void_t<decltype(std::declval<const R&>().value_or(std::declval<U>()))>>
: std::true_type {};

template <class R, class U, class = void>
struct HasEmplace : std::false_type {};

template <class R, class U>
struct HasEmplace<R, U, std::void_t<decltype(std::declval<R&>().emplace(std::declval<U>()))>>
: std::true_type {};

static_assert(!ros2_bag::is_result_error_v<Incomplete>);
static_assert(!ros2_bag::is_result_error_v<AbstractType>);
static_assert(ros2_bag::is_result_error_v<ErrorCode>);
static_assert(!ros2_bag::is_result_error_v<const ErrorCode>);
static_assert(!ros2_bag::is_result_error_v<ros2_bag::Unexpected<ErrorCode>>);

static_assert(ros2_bag::is_result_v<ros2_bag::Result<int, ErrorCode>>);
static_assert(ros2_bag::is_result_v<const ros2_bag::Result<int, ErrorCode>&>);
static_assert(!ros2_bag::is_result_v<int>);

static_assert(std::is_trivially_copyable_v<ros2_bag::Unexpected<int>>);
static_assert(std::is_nothrow_move_constructible_v<ros2_bag::Unexpected<int>>);

static_assert(!std::is_default_constructible_v<ros2_bag::Result<ErrorNoDefault, ErrorCode>>);
static_assert(std::is_default_constructible_v<ros2_bag::Result<void, ErrorNoDefault>>);

using TrivialResult = ros2_bag::Result<int, ErrorCode>;
using TrivialVoidResult = ros2_bag::Result<void, ErrorCode>;

static_assert(std::is_trivially_destructible_v<TrivialResult>);
static_assert(std::is_trivially_copy_constructible_v<TrivialResult>);
static_assert(std::is_trivially_move_constructible_v<TrivialResult>);
static_assert(std::is_trivially_copy_assignable_v<TrivialResult>);
static_assert(std::is_trivially_move_assignable_v<TrivialResult>);
static_assert(std::is_trivially_copyable_v<TrivialResult>);

static_assert(std::is_trivially_destructible_v<TrivialVoidResult>);
static_assert(std::is_trivially_copy_constructible_v<TrivialVoidResult>);
static_assert(std::is_trivially_move_constructible_v<TrivialVoidResult>);
static_assert(std::is_trivially_copy_assignable_v<TrivialVoidResult>);
static_assert(std::is_trivially_move_assignable_v<TrivialVoidResult>);
static_assert(std::is_trivially_copyable_v<TrivialVoidResult>);

using MoveOnlyResult = ros2_bag::Result<MoveOnlyTrivial, ErrorCode>;
static_assert(!std::is_copy_constructible_v<MoveOnlyResult>);
static_assert(std::is_move_constructible_v<MoveOnlyResult>);
static_assert(!std::is_copy_assignable_v<MoveOnlyResult>);
static_assert(std::is_move_assignable_v<MoveOnlyResult>);

using CopyOnlyResult = ros2_bag::Result<CopyOnlyTrivial, CopyOnlyTrivial>;
static_assert(std::is_copy_constructible_v<CopyOnlyResult>);
// The move traits ask whether an rvalue expression is accepted. A
// defaulted-deleted move special member can be ignored, leaving the
// copy special member as a valid fallback.
static_assert(std::is_move_constructible_v<CopyOnlyResult>);
static_assert(std::is_copy_assignable_v<CopyOnlyResult>);
static_assert(std::is_move_assignable_v<CopyOnlyResult>);

using MixedTrivialResult = ros2_bag::Result<CopyOnlyTrivial, MoveOnlyTrivial>;
static_assert(!std::is_copy_constructible_v<MixedTrivialResult>);
static_assert(!std::is_move_constructible_v<MixedTrivialResult>);
static_assert(std::is_trivially_copyable_v<MixedTrivialResult>);

using ThrowingMoveResult = ros2_bag::Result<ThrowingMove, ThrowingMove>;
static_assert(std::is_copy_constructible_v<ThrowingMoveResult>);
static_assert(std::is_move_constructible_v<ThrowingMoveResult>);
// Copy assignment remains safe through Strategy A because both copy
// constructions are noexcept, even though both move constructions can throw.
static_assert(std::is_copy_assignable_v<ThrowingMoveResult>);
static_assert(std::is_nothrow_copy_assignable_v<ThrowingMoveResult>);
// The custom move-assignment path is unavailable, but the noexcept
// copy assignment can still accept an rvalue Result.
static_assert(std::is_move_assignable_v<ThrowingMoveResult>);

static_assert(std::is_nothrow_copy_constructible_v<TrivialResult>);
static_assert(std::is_nothrow_move_constructible_v<TrivialResult>);
static_assert(std::is_nothrow_copy_assignable_v<TrivialResult>);
static_assert(std::is_nothrow_move_assignable_v<TrivialResult>);

static_assert(!std::is_nothrow_copy_constructible_v<ros2_bag::Result<std::string, ErrorCode>>);
static_assert(!std::is_nothrow_copy_assignable_v<ros2_bag::Result<std::string, ErrorCode>>);

using StringVoidResult = ros2_bag::Result<void, std::string>;
static_assert(std::is_copy_constructible_v<StringVoidResult>);
static_assert(std::is_move_constructible_v<StringVoidResult>);
static_assert(std::is_copy_assignable_v<StringVoidResult>);
static_assert(std::is_move_assignable_v<StringVoidResult>);
static_assert(!std::is_trivially_copyable_v<StringVoidResult>);
static_assert(!std::is_nothrow_copy_assignable_v<StringVoidResult>);
static_assert(std::is_nothrow_move_assignable_v<StringVoidResult>);

using MoveOnlyVoidResult = ros2_bag::Result<void, MoveOnlyError>;
static_assert(!std::is_copy_constructible_v<MoveOnlyVoidResult>);
static_assert(std::is_move_constructible_v<MoveOnlyVoidResult>);
static_assert(!std::is_copy_assignable_v<MoveOnlyVoidResult>);
static_assert(std::is_move_assignable_v<MoveOnlyVoidResult>);

// Direct value assignment is not a safe-reinit template candidate, but
// implicit construction of a temporary Result followed by its safe copy
// assignment still makes the overall assignment expression well-formed.
static_assert(std::is_assignable_v<ThrowingMoveResult&, ThrowingMove>);

static_assert(std::is_constructible_v<
              ros2_bag::Result<FromUnexpect, ErrorCode>, std::in_place_t, ros2_bag::unexpect_t>);
static_assert(
    !std::is_constructible_v<ros2_bag::Result<FromUnexpect, ErrorNoDefault>, ros2_bag::unexpect_t>);

static_assert(std::is_convertible_v<int, ros2_bag::Result<ImplicitValue, ErrorCode>>);
static_assert(!std::is_convertible_v<int, ros2_bag::Result<ExplicitValue, ErrorCode>>);

using IntUnexpected = ros2_bag::Unexpected<int>;
static_assert(std::is_convertible_v<IntUnexpected, ros2_bag::Result<int, ImplicitError>>);
static_assert(!std::is_convertible_v<IntUnexpected, ros2_bag::Result<int, ExplicitError>>);

using SourceResult = ros2_bag::Result<int, ErrorCode>;
static_assert(!std::is_constructible_v<ros2_bag::Result<bool, ErrorCode>, SourceResult&>);
static_assert(std::is_constructible_v<ros2_bag::Result<DirectWrapper, ErrorCode>, SourceResult&>);

static_assert(HasConstValueOr<ros2_bag::Result<ImplicitValue, ErrorCode>, int>::value);
static_assert(!HasConstValueOr<ros2_bag::Result<ExplicitValue, ErrorCode>, int>::value);

// value_or intentionally follows std::expected: convertibility is required,
// and no conditional noexcept contract is exposed.
static_assert(!noexcept(std::declval<const TrivialResult&>().value_or(0)));
static_assert(!noexcept(std::declval<TrivialResult&&>().value_or(0)));

static_assert(HasEmplace<ros2_bag::Result<ImplicitValue, ErrorCode>, int>::value);
static_assert(!HasEmplace<ros2_bag::Result<StrategyBValue, ErrorCode>, ConversionInput>::value);

constexpr auto kUnexpected = ros2_bag::make_unexpected(42);
static_assert(kUnexpected.error() == 42);

void test_basic_construction_and_access() {
    ros2_bag::Result<int, ErrorCode> value = 42;
    CHECK(value);
    CHECK(value.has_value());
    CHECK(value.value() == 42);
    CHECK(*value == 42);
    CHECK(value.operator->() != nullptr);

    ros2_bag::Result<int, ErrorCode> error(ros2_bag::unexpect, ErrorCode::Two);
    CHECK(!error);
    CHECK(!error.has_value());
    CHECK(error.error() == ErrorCode::Two);

    ros2_bag::Result<void, ErrorCode> success;
    CHECK(success.has_value());
    success.value();
    *success;

    ros2_bag::Result<void, ErrorCode> void_error(ros2_bag::unexpect, ErrorCode::One);
    CHECK(!void_error);
    CHECK(void_error.error() == ErrorCode::One);

    ros2_bag::Result<int, int> same_value = 7;
    ros2_bag::Result<int, int> same_error(ros2_bag::unexpect, 9);
    CHECK(same_value.value() == 7);
    CHECK(same_error.error() == 9);

    using Inner = ros2_bag::Result<int, ErrorCode>;
    using Outer = ros2_bag::Result<Inner, int>;

    Outer nested(Inner{11});
    CHECK(nested.has_value());
    CHECK(nested->value() == 11);

    ros2_bag::Result<int, ros2_bag::Result<void, ErrorCode>> result_as_error(
        ros2_bag::unexpect, ros2_bag::Result<void, ErrorCode>{});
    CHECK(!result_as_error);
    CHECK(result_as_error.error().has_value());
}

void test_unexpected_and_tag_boundaries() {
    auto unexpected = ros2_bag::make_unexpected(std::string{"failure"});
    CHECK(unexpected.error() == "failure");

    ros2_bag::Result<TagValue, ErrorNoDefault> from_in_place(std::in_place, ros2_bag::unexpect);
    CHECK(from_in_place->value == 2);

    ros2_bag::Result<TagValue, BackupError> assigned(std::in_place, ros2_bag::unexpect);
    assigned = std::in_place;
    CHECK(assigned->value == 3);
    assigned = ros2_bag::unexpect;
    CHECK(assigned->value == 4);

    ros2_bag::Result<int, ImplicitError> implicit_error = ros2_bag::make_unexpected(21);
    CHECK(!implicit_error);
    CHECK(implicit_error.error().value == 21);

    ros2_bag::Result<int, ExplicitError> explicit_error(ros2_bag::make_unexpected(22));
    CHECK(!explicit_error);
    CHECK(explicit_error.error().value == 22);
}

void test_trivial_special_member_state_switching() {
    ros2_bag::Result<int, ErrorCode> value = 10;
    ros2_bag::Result<int, ErrorCode> error(ros2_bag::unexpect, ErrorCode::One);

    error = value;
    CHECK(error.has_value());
    CHECK(error.value() == 10);

    value = ros2_bag::Result<int, ErrorCode>(ros2_bag::unexpect, ErrorCode::Two);
    CHECK(!value.has_value());
    CHECK(value.error() == ErrorCode::Two);

    ros2_bag::Result<int, ErrorCode> moved_value = 20;
    value = std::move(moved_value);
    CHECK(value.has_value());
    CHECK(value.value() == 20);
    CHECK(moved_value.has_value());

    ros2_bag::Result<void, ErrorCode> void_success;
    ros2_bag::Result<void, ErrorCode> void_error(ros2_bag::unexpect, ErrorCode::One);

    void_error = void_success;
    CHECK(void_error.has_value());

    void_success = ros2_bag::Result<void, ErrorCode>(ros2_bag::unexpect, ErrorCode::Two);
    CHECK(!void_success.has_value());
    CHECK(void_success.error() == ErrorCode::Two);

    ros2_bag::Result<void, std::string> string_success;
    ros2_bag::Result<void, std::string> string_error(ros2_bag::unexpect, "copy");

    string_error = string_success;
    CHECK(string_error.has_value());

    string_success = ros2_bag::Result<void, std::string>(ros2_bag::unexpect, "move");
    string_error = std::move(string_success);
    CHECK(!string_error.has_value());
    CHECK(string_error.error() == "move");
    CHECK(!string_success.has_value());
}

void test_copy_move_and_lifetime() {
    Lifetime::reset();

    {
        ros2_bag::Result<Lifetime, Lifetime> value(std::in_place, 10);
        ros2_bag::Result<Lifetime, Lifetime> error(ros2_bag::unexpect, 20);

        CHECK(Lifetime::live_count == 2);

        auto value_copy = value;
        auto error_copy = error;
        CHECK(value_copy->value == 10);
        CHECK(error_copy.error().value == 20);
        CHECK(Lifetime::live_count == 4);

        auto value_move = std::move(value_copy);
        auto error_move = std::move(error_copy);
        CHECK(value_move->value == 10);
        CHECK(error_move.error().value == 20);
        CHECK(value_copy.has_value());
        CHECK(!error_copy.has_value());

        value_move = value;
        error_move = error;
        CHECK(value_move->value == 10);
        CHECK(error_move.error().value == 20);

        auto* value_alias = std::addressof(value_move);
        auto* error_alias = std::addressof(error_move);
        value_move = std::move(*value_alias);
        error_move = std::move(*error_alias);
        CHECK(value_move.has_value());
        CHECK(!error_move.has_value());
    }

    CHECK(Lifetime::live_count == 0);
}

void test_result_cross_state_assignment_rollback() {
    ThrowingCopyValue::throw_on_copy = false;

    ros2_bag::Result<ThrowingCopyValue, BackupError> source_value(std::in_place, 31);
    ros2_bag::Result<ThrowingCopyValue, BackupError> target_error(ros2_bag::unexpect, 32);

    ThrowingCopyValue::throw_on_copy = true;

    try {
        target_error = source_value;
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(!target_error.has_value());
        CHECK(target_error.error().value == 32);
        CHECK(source_value.has_value());
        CHECK(source_value->value == 31);
    }

    ThrowingCopyValue::throw_on_copy = false;
    target_error = source_value;
    CHECK(target_error.has_value());
    CHECK(target_error->value == 31);

    ros2_bag::Result<BackupValue, ThrowingCopyValue> source_error(ros2_bag::unexpect, 41);
    ros2_bag::Result<BackupValue, ThrowingCopyValue> target_value(std::in_place, 42);

    ThrowingCopyValue::throw_on_copy = true;

    try {
        target_value = source_error;
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(target_value.has_value());
        CHECK(target_value->value == 42);
        CHECK(!source_error.has_value());
        CHECK(source_error.error().value == 41);
    }

    ThrowingCopyValue::throw_on_copy = false;

    ThrowingMoveError::throw_on_move = false;
    ros2_bag::Result<ThrowingMoveError, BackupError> move_source(std::in_place, 51);
    ros2_bag::Result<ThrowingMoveError, BackupError> move_target(ros2_bag::unexpect, 52);

    ThrowingMoveError::throw_on_move = true;

    try {
        move_target = std::move(move_source);
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(!move_target.has_value());
        CHECK(move_target.error().value == 52);
        CHECK(move_source.has_value());
        CHECK(move_source->value == -1);
    }

    ThrowingMoveError::throw_on_move = false;
}

void test_same_state_assignment_exception() {
    ros2_bag::Result<ThrowingAssign, BackupError> value(std::in_place, 7);

    ThrowingAssign replacement{9};

    try {
        value = replacement;
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(value.has_value());
        CHECK(value->value == 7);
    }

    ros2_bag::Result<BackupValue, ThrowingAssign> error(ros2_bag::unexpect, 3);

    auto replacement_error = ros2_bag::make_unexpected(ThrowingAssign{4});

    try {
        error = replacement_error;
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(!error.has_value());
        CHECK(error.error().value == 3);
    }
}

void test_reinit_error_to_value() {
    StrategyAValue::reset();

    {
        ros2_bag::Result<StrategyAValue, BackupError> result(ros2_bag::unexpect, 10);

        result = ConversionInput{20, false};

        CHECK(result.has_value());
        CHECK(result->value == 20);
        CHECK(StrategyAValue::conversion_count == 1);
        CHECK(StrategyAValue::move_count == 0);
    }

    StrategyBValue::reset();

    {
        ros2_bag::Result<StrategyBValue, BackupError> result(ros2_bag::unexpect, 30);

        result = ConversionInput{40, false};

        CHECK(result.has_value());
        CHECK(result->value == 40);
        CHECK(StrategyBValue::conversion_count == 1);
        CHECK(StrategyBValue::move_count == 1);
    }

    StrategyBValue::reset();

    {
        ros2_bag::Result<StrategyBValue, BackupError> result(ros2_bag::unexpect, 50);

        try {
            result = ConversionInput{60, true};
            CHECK(false);
        } catch (const TestFailure&) {
            CHECK(!result.has_value());
            CHECK(result.error().value == 50);
            CHECK(StrategyBValue::move_count == 0);
        }
    }

    StrategyCValue::reset();
    BackupError::reset();

    {
        ros2_bag::Result<StrategyCValue, BackupError> result(ros2_bag::unexpect, 70);

        try {
            result = ConversionInput{80, true};
            CHECK(false);
        } catch (const TestFailure&) {
            CHECK(!result.has_value());
            CHECK(result.error().value == 70);
            CHECK(BackupError::move_count == 2);
        }
    }
}

template <class Error>
void check_value_to_error_success(int initial_value, ConversionInput input, int expected_error) {
    ros2_bag::Result<BackupValue, Error> result(std::in_place, initial_value);

    result = ros2_bag::make_unexpected(input);

    CHECK(!result.has_value());
    CHECK(result.error().value == expected_error);
}

void test_reinit_value_to_error() {
    StrategyAValue::reset();
    check_value_to_error_success<StrategyAValue>(1, ConversionInput{2, false}, 2);
    CHECK(StrategyAValue::move_count == 0);

    StrategyBValue::reset();
    check_value_to_error_success<StrategyBValue>(3, ConversionInput{4, false}, 4);
    CHECK(StrategyBValue::move_count == 1);

    StrategyCValue::reset();
    BackupValue::reset();

    {
        ros2_bag::Result<BackupValue, StrategyCValue> result(std::in_place, 5);

        try {
            result = ros2_bag::make_unexpected(ConversionInput{6, true});
            CHECK(false);
        } catch (const TestFailure&) {
            CHECK(result.has_value());
            CHECK(result->value == 5);
            CHECK(BackupValue::move_count == 2);
        }
    }
}

void test_void_error_assignment_rollback() {
    StrategyBValue::reset();

    ros2_bag::Result<void, StrategyBValue> result;
    CHECK(result.has_value());

    try {
        result = ros2_bag::make_unexpected(ConversionInput{91, true});
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(result.has_value());
        CHECK(StrategyBValue::move_count == 0);
    }

    result = ros2_bag::make_unexpected(ConversionInput{92, false});
    CHECK(!result.has_value());
    CHECK(result.error().value == 92);

    try {
        result = ros2_bag::make_unexpected(ConversionInput{93, true});
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(!result.has_value());
        CHECK(result.error().value == 92);
    }

    result.emplace();
    CHECK(result.has_value());
}

void test_value_or_and_emplace() {
    ros2_bag::Result<int, ErrorCode> value = 5;
    ros2_bag::Result<int, ErrorCode> error(ros2_bag::unexpect, ErrorCode::One);

    CHECK(value.value_or(9) == 5);
    CHECK(error.value_or(9) == 9);

    error.emplace(11);
    CHECK(error.has_value());
    CHECK(error.value() == 11);

    value.emplace(12);
    CHECK(value.value() == 12);

    ros2_bag::Result<void, ErrorCode> void_error(ros2_bag::unexpect, ErrorCode::One);
    void_error.emplace();
    CHECK(void_error.has_value());
}

void test_monadic_non_void() {
    ros2_bag::Result<int, ErrorCode> value = 10;

    auto and_then = value.and_then([](int& input) {
        input += 1;
        return ros2_bag::Result<long, ErrorCode>{static_cast<long>(input * 2)};
    });
    CHECK(and_then.value() == 22);
    CHECK(value.value() == 11);

    const ros2_bag::Result<int, ErrorCode> const_value = 20;
    auto const_and_then = const_value.and_then([](const int& input) {
        return ros2_bag::Result<long, ErrorCode>{static_cast<long>(input + 1)};
    });
    CHECK(const_and_then.value() == 21);

    ros2_bag::Result<int, ErrorCode> rvalue = 30;
    auto rvalue_and_then = std::move(rvalue).and_then(
        [](int&& input) { return ros2_bag::Result<int, ErrorCode>{input + 1}; });
    CHECK(rvalue_and_then.value() == 31);

    const ros2_bag::Result<int, ErrorCode> const_rvalue = 40;
    auto const_rvalue_and_then = std::move(const_rvalue).and_then([](const int&& input) {
        return ros2_bag::Result<int, ErrorCode>{input + 1};
    });
    CHECK(const_rvalue_and_then.value() == 41);

    ros2_bag::Result<long, ErrorCode> shared = 88;
    auto normalized_ref =
        value.and_then([&](int&) -> ros2_bag::Result<long, ErrorCode>& { return shared; });
    static_assert(std::is_same_v<decltype(normalized_ref), ros2_bag::Result<long, ErrorCode>>);
    CHECK(normalized_ref.value() == 88);

    auto transformed = value.transform([](int input) { return input + 5; });
    CHECK(transformed.value() == 16);

    auto transformed_void = value.transform([](int) {});
    static_assert(std::is_same_v<decltype(transformed_void), ros2_bag::Result<void, ErrorCode>>);
    CHECK(transformed_void.has_value());

    auto transformed_const = value.transform(ConstBoxCallable{});
    static_assert(std::is_same_v<decltype(transformed_const), ros2_bag::Result<Box, ErrorCode>>);
    CHECK(transformed_const->value == 12);

    ros2_bag::Result<int, ErrorCode> error(ros2_bag::unexpect, ErrorCode::Two);

    bool and_then_called = false;
    auto propagated = error.and_then([&](int&) {
        and_then_called = true;
        return ros2_bag::Result<long, ErrorCode>{1};
    });
    CHECK(!and_then_called);
    CHECK(!propagated.has_value());
    CHECK(propagated.error() == ErrorCode::Two);

    auto recovered = error.or_else([](ErrorCode code) {
        return ros2_bag::Result<int, int>(ros2_bag::unexpect, code == ErrorCode::Two ? 9 : 0);
    });
    CHECK(!recovered.has_value());
    CHECK(recovered.error() == 9);

    auto changed_error = error.transform_error([](ErrorCode code) {
        return code == ErrorCode::Two ? std::string{"two"} : std::string{"one"};
    });
    CHECK(!changed_error.has_value());
    CHECK(changed_error.error() == "two");

    auto preserved_success = value.transform_error([](ErrorCode) { return std::string{"error"}; });
    CHECK(preserved_success.has_value());
    CHECK(preserved_success.value() == 11);
}

void test_monadic_void() {
    ros2_bag::Result<void, ErrorCode> success;

    bool invoked = false;
    auto next = success.and_then([&] {
        invoked = true;
        return ros2_bag::Result<int, ErrorCode>{7};
    });
    CHECK(invoked);
    CHECK(next.value() == 7);

    auto transformed = success.transform([] { return 12; });
    CHECK(transformed.value() == 12);

    auto transformed_void = success.transform([] {});
    CHECK(transformed_void.has_value());

    ros2_bag::Result<void, ErrorCode> error(ros2_bag::unexpect, ErrorCode::One);

    auto changed = error.transform_error([](ErrorCode) { return std::string{"failure"}; });
    CHECK(!changed.has_value());
    CHECK(changed.error() == "failure");

    auto recovered = error.or_else(
        [](ErrorCode) { return ros2_bag::Result<void, int>(ros2_bag::unexpect, 55); });
    CHECK(!recovered.has_value());
    CHECK(recovered.error() == 55);
}

void test_monadic_exception_semantics() {
    ros2_bag::Result<int, ErrorCode> value = 1;

    try {
        (void)value.transform([](int& input) -> int {
            input = 99;
            throw TestFailure{};
        });
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(value.has_value());
        CHECK(value.value() == 99);
    }

    ThrowingMoveError::throw_on_move = false;
    ros2_bag::Result<int, ThrowingMoveError> error(ros2_bag::unexpect, 77);

    ThrowingMoveError::throw_on_move = true;

    try {
        (void)std::move(error).and_then(
            [](int&&) { return ros2_bag::Result<long, ThrowingMoveError>{1}; });
        CHECK(false);
    } catch (const TestFailure&) {
        CHECK(!error.has_value());
        CHECK(error.error().value == -1);
    }

    ThrowingMoveError::throw_on_move = false;
}

void test_fail_fast() {
    CHECK(terminates([] {
        ros2_bag::Result<int, ErrorCode> result(ros2_bag::unexpect, ErrorCode::One);
        (void)result.value();
    }));

    CHECK(terminates([] {
        ros2_bag::Result<int, ErrorCode> result(ros2_bag::unexpect, ErrorCode::One);
        (void)*result;
    }));

    CHECK(terminates([] {
        ros2_bag::Result<int, ErrorCode> result(ros2_bag::unexpect, ErrorCode::One);
        (void)result.operator->();
    }));

    CHECK(terminates([] {
        ros2_bag::Result<int, ErrorCode> result = 1;
        (void)result.error();
    }));

    CHECK(terminates([] {
        ros2_bag::Result<void, ErrorCode> result(ros2_bag::unexpect, ErrorCode::One);
        result.value();
    }));

    CHECK(terminates([] {
        ros2_bag::Result<void, ErrorCode> result(ros2_bag::unexpect, ErrorCode::One);
        *result;
    }));

    CHECK(terminates([] {
        ros2_bag::Result<void, ErrorCode> result;
        (void)result.error();
    }));
}

}  // namespace

int main() {
    test_basic_construction_and_access();
    test_unexpected_and_tag_boundaries();
    test_trivial_special_member_state_switching();
    test_copy_move_and_lifetime();
    test_result_cross_state_assignment_rollback();
    test_same_state_assignment_exception();
    test_reinit_error_to_value();
    test_reinit_value_to_error();
    test_void_error_assignment_rollback();
    test_value_or_and_emplace();
    test_monadic_non_void();
    test_monadic_void();
    test_monadic_exception_semantics();
    test_fail_fast();

    if (g_failures != 0) {
        std::cerr << g_failures << " Result test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All Result tests passed\n";
    return EXIT_SUCCESS;
}
