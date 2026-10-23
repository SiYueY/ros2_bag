#!/usr/bin/env python3
"""Apply project clang-format plus the requested one-argument-per-line rule.

clang-format 14 bin-packing options still keep short calls on one line. This
small whitespace-only pass expands multi-argument parentheses, including nested
calls, without touching string literals/comments or third-party source.
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[()\[\]{},]')
STYLE = ('{BasedOnStyle: InheritParentConfig, BinPackArguments: false, '
         'BinPackParameters: false, AllowAllArgumentsOnNextLine: false, '
         'AllowAllParametersOfDeclarationOnNextLine: false, '
         'ConstructorInitializerAllOnOneLineOrOnePerLine: true}')


def expand_arguments(text):
    stack = []
    groups = []
    for match in TOKENS.finditer(text):
        token = match.group()
        if token in ('(', '[', '{'):
            stack.append([token, match.start(), []])
        elif token in (')', ']', '}'):
            if stack:
                opening, position, commas = stack.pop()
                if opening == '(' and token == ')' and commas:
                    groups.append((position, commas, match.start()))
        elif token == ',' and stack and stack[-1][0] == '(':
            stack[-1][2].append(match.start())
    edits = []
    for opening, commas, closing in groups:
        line_start = text.rfind('\n', 0, opening) + 1
        indent = re.match(r' *', text[line_start:]).group()
        starts = [opening + 1] + [comma + 1 for comma in commas]
        for start in starts:
            end = start
            while end < len(text) and text[end].isspace():
                end += 1
            edits.append((start, end, '\n' + indent + '    '))
        start = closing
        while start > 0 and text[start - 1].isspace():
            start -= 1
        edits.append((start, closing, '\n' + indent))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def formatted(path):
    result = subprocess.check_output(['clang-format', '-style=' + STYLE, str(path)], text=True)
    # Constructor initializer lists also need one item per line even when short.
    edits = []
    for match in re.finditer(r'(?m)^( *): *', result):
        depth = 0
        indent = match.group(1) + '  '
        for token in TOKENS.finditer(result, match.end()):
            symbol = token.group()
            if symbol == '(':
                depth += 1
            elif symbol == ')':
                depth -= 1
            elif symbol == '{' and depth == 0:
                break
            elif symbol == ',' and depth == 0:
                start = token.end()
                end = start
                while result[end].isspace():
                    end += 1
                edits.append((start, end, '\n' + indent))
    for start, end, replacement in sorted(edits, reverse=True):
        result = result[:start] + replacement + result[end:]
    return expand_arguments(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    changed = []
    for folder in ('include', 'src', 'tests'):
        for path in sorted((ROOT / folder).rglob('*')):
            if path.suffix not in ('.cpp', '.hpp'):
                continue
            result = formatted(path)
            if path.read_text() != result:
                changed.append(str(path.relative_to(ROOT)))
                if not args.check:
                    path.write_text(result)
    if args.check and changed:
        parser.exit(1, 'Formatting differs: ' + ', '.join(changed) + '\n')
    print('PASS C++ formatting' if args.check else f'Formatted {len(changed)} files')


if __name__ == '__main__':
    main()
