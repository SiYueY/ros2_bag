#!/usr/bin/env python3
"""Apply the repository clang-format configuration without altering syntax.

The project's clang-format configuration contains the argument-packing rules.
Do not add a text-level postprocessor here: C++ braced initializers and nested
calls cannot safely be rewritten without parsing C++, and formatting must never
change a protocol string or other source token.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
STYLE = ('{BasedOnStyle: InheritParentConfig, BinPackArguments: false, '
         'BinPackParameters: false, AllowAllArgumentsOnNextLine: false, '
         'AllowAllParametersOfDeclarationOnNextLine: false, '
         'ConstructorInitializerAllOnOneLineOrOnePerLine: true}')

def formatted(path):
    result = subprocess.check_output(['clang-format', '-style=' + STYLE, str(path)], text=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    changed = []
    for folder in ('include', 'src', 'tests'):
        for path in sorted((ROOT / folder).rglob('*')):
            if path.suffix not in ('.cpp', '.cc', '.h', '.hpp'):
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
