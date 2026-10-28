#!/usr/bin/env python3
"""Check repository C++ rules that clang-format cannot enforce."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP_SUFFIXES = {".cpp", ".cc", ".h", ".hpp"}
CONTROL = re.compile(r"^(?:else\s+)?(?:if|for|while)\s*\(")
SIMPLE_CONTROL = re.compile(r"^(?:else|do)\b")
TERNARY = re.compile(r"\?[^:;]+:")


def source_files():
    for folder in ("include", "src", "tests"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.suffix in CPP_SUFFIXES:
                yield path


def strip_line_comment(line):
    # The check is intentionally narrow: it only needs enough lexical handling to
    # identify the opening token after an if/for/while condition.
    return line.split("//", 1)[0]


def control_violations(path):
    lines = path.read_text().splitlines()
    violations = []
    index = 0
    while index < len(lines):
        stripped = strip_line_comment(lines[index]).strip()
        while stripped.startswith("}"):
            stripped = stripped[1:].lstrip()

        simple = SIMPLE_CONTROL.match(stripped)
        if simple and not stripped.startswith("else if"):
            remainder = stripped[simple.end():].strip()
            if remainder:
                if not remainder.startswith("{"):
                    violations.append(index + 1)
            else:
                next_index = index + 1
                while next_index < len(lines) and not strip_line_comment(lines[next_index]).strip():
                    next_index += 1
                if next_index < len(lines):
                    if not strip_line_comment(lines[next_index]).lstrip().startswith("{"):
                        violations.append(index + 1)
            index += 1
            continue

        match = CONTROL.match(stripped)
        if not match:
            index += 1
            continue

        depth = 0
        saw_open = False
        end_index = index
        end_offset = None
        while end_index < len(lines):
            current = strip_line_comment(lines[end_index])
            start = 0
            if end_index == index:
                keyword_end = match.end() - 1
                start = keyword_end
            for offset in range(start, len(current)):
                char = current[offset]
                if char == "(":
                    depth += 1
                    saw_open = True
                elif char == ")" and saw_open:
                    depth -= 1
                    if depth == 0:
                        end_offset = offset + 1
                        break
            if end_offset is not None:
                break
            end_index += 1

        if end_offset is None:
            index += 1
            continue

        remainder = strip_line_comment(lines[end_index])[end_offset:].strip()
        if remainder:
            if not remainder.startswith("{"):
                violations.append(index + 1)
        else:
            next_index = end_index + 1
            while next_index < len(lines) and not strip_line_comment(lines[next_index]).strip():
                next_index += 1
            if next_index < len(lines):
                if not strip_line_comment(lines[next_index]).lstrip().startswith("{"):
                    violations.append(index + 1)
        index = end_index + 1

    return violations


def main():
    failed = False
    for path in source_files():
        relative = path.relative_to(ROOT)
        for line in control_violations(path):
            failed = True
            print(
                f"{relative}:{line}: if/for/while body must use explicit braces",
                file=sys.stderr,
            )

        if relative.parts[0] in ("include", "src"):
            for line_number, line in enumerate(path.read_text().splitlines(), 1):
                if TERNARY.search(strip_line_comment(line)):
                    failed = True
                    print(
                        f"{relative}:{line_number}: production C++ must not use ternary expressions",
                        file=sys.stderr,
                    )

    if failed:
        return 1
    print("PASS C++ semantic style checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
