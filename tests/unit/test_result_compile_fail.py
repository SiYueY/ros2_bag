#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import tempfile
import textwrap


CASES = {
    "abstract_value_type": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class Error { Failed };

        struct Abstract {
            virtual ~Abstract() noexcept = default;
            virtual void run() = 0;
        };

        ros2_bag::Result<Abstract, Error> result;

        int main() {
            return 0;
        }
        """,
        "Result<T,E> requires T",
    ),
    "transform_reference_result": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class Error { Failed };

        int main() {
            ros2_bag::Result<int, Error> result = 1;
            int output = 0;
            auto invalid = result.transform(
                [&](int&) -> int& {
                    return output;
                });
            (void)invalid;
        }
        """,
        "Result::transform requires a non-reference valid value type or void",
    ),
    "and_then_non_result": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class Error { Failed };

        int main() {
            ros2_bag::Result<int, Error> result = 1;
            auto invalid = result.and_then(
                [](int&) {
                    return 2;
                });
            (void)invalid;
        }
        """,
        "Result::and_then requires F to return Result<U,E>",
    ),
    "and_then_changes_error": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class ErrorA { Failed };
        enum class ErrorB { Failed };

        int main() {
            ros2_bag::Result<int, ErrorA> result = 1;
            auto invalid = result.and_then(
                [](int&) {
                    return ros2_bag::Result<long, ErrorB>{2L};
                });
            (void)invalid;
        }
        """,
        "Result::and_then cannot change error_type",
    ),
    "and_then_cannot_propagate_lvalue_error": (
        r"""
        #include "ros2_bag/result.hpp"

        struct MoveOnlyError {
            MoveOnlyError() = default;
            MoveOnlyError(const MoveOnlyError&) = delete;
            MoveOnlyError& operator=(const MoveOnlyError&) = delete;
            MoveOnlyError(MoveOnlyError&&) noexcept = default;
            MoveOnlyError& operator=(MoveOnlyError&&) noexcept = default;
            ~MoveOnlyError() noexcept = default;
        };

        int main() {
            ros2_bag::Result<int, MoveOnlyError> result(
                ros2_bag::unexpect);
            auto invalid = result.and_then(
                [](int&) {
                    return ros2_bag::Result<long, MoveOnlyError>{2L};
                });
            (void)invalid;
        }
        """,
        "Result::and_then requires error propagation",
    ),
    "or_else_changes_value_type": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class ErrorA { Failed };
        enum class ErrorB { Failed };

        int main() {
            ros2_bag::Result<int, ErrorA> result(
                ros2_bag::unexpect,
                ErrorA::Failed);
            auto invalid = result.or_else(
                [](ErrorA&) {
                    return ros2_bag::Result<long, ErrorB>{2L};
                });
            (void)invalid;
        }
        """,
        "Result::or_else cannot change value_type",
    ),
    "transform_error_void": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class Error { Failed };

        int main() {
            ros2_bag::Result<int, Error> result(
                ros2_bag::unexpect,
                Error::Failed);
            auto invalid = result.transform_error(
                [](Error&) {});
            (void)invalid;
        }
        """,
        "Result::transform_error requires a valid non-reference error type",
    ),
    "transform_target_not_constructible": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class Error { Failed };

        struct NonMovable {
            NonMovable() = default;
            NonMovable(const NonMovable&) = delete;
            NonMovable(NonMovable&&) = delete;
            NonMovable& operator=(const NonMovable&) = delete;
            NonMovable& operator=(NonMovable&&) = delete;
            ~NonMovable() noexcept = default;
        };

        int main() {
            ros2_bag::Result<int, Error> result = 1;
            auto invalid = result.transform(
                [](int&) -> NonMovable {
                    return NonMovable{};
                });
            (void)invalid;
        }
        """,
        "Result::transform target value must be constructible from F's return type",
    ),
    "transform_error_target_not_constructible": (
        r"""
        #include "ros2_bag/result.hpp"

        enum class Error { Failed };

        struct NonMovableError {
            NonMovableError() = default;
            NonMovableError(const NonMovableError&) = delete;
            NonMovableError(NonMovableError&&) = delete;
            NonMovableError& operator=(const NonMovableError&) = delete;
            NonMovableError& operator=(NonMovableError&&) = delete;
            ~NonMovableError() noexcept = default;
        };

        int main() {
            ros2_bag::Result<int, Error> result(
                ros2_bag::unexpect,
                Error::Failed);
            auto invalid = result.transform_error(
                [](Error&) -> NonMovableError {
                    return NonMovableError{};
                });
            (void)invalid;
        }
        """,
        "Result::transform_error target error must be constructible from F's return type",
    ),
}


def compile_case(
    compiler: str,
    include_dir: pathlib.Path,
    name: str,
    source: str,
    expected_diagnostic: str,
) -> bool:
    with tempfile.TemporaryDirectory(
        prefix=f"result_compile_fail_{name}_"
    ) as temp_dir:
        temp_path = pathlib.Path(temp_dir)
        source_path = temp_path / f"{name}.cpp"
        object_path = temp_path / f"{name}.o"
        source_path.write_text(
            textwrap.dedent(source),
            encoding="utf-8",
        )

        command = [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-I",
            str(include_dir),
            "-c",
            str(source_path),
            "-o",
            str(object_path),
        ]

        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )

        combined = completed.stdout + completed.stderr

        if completed.returncode == 0:
            print(
                f"[FAIL] {name}: compilation unexpectedly succeeded"
            )
            return False

        if expected_diagnostic not in combined:
            print(
                f"[FAIL] {name}: expected diagnostic not found: "
                f"{expected_diagnostic!r}"
            )
            print(combined)
            return False

        print(f"[PASS] {name}")
        return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--compiler",
        required=True,
    )
    parser.add_argument(
        "--include",
        required=True,
        type=pathlib.Path,
    )
    args = parser.parse_args()

    ok = True

    for name, (
        source,
        expected_diagnostic,
    ) in CASES.items():
        ok = (
            compile_case(
                args.compiler,
                args.include,
                name,
                source,
                expected_diagnostic,
            )
            and ok
        )

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
