#!/usr/bin/env python3
"""Reformat files in place with clang-format.

Used by the `format` build target (see cmake/format.cmake). Formatting is a
convenience, not a correctness gate: a file clang-format cannot parse is
reported on stderr and skipped instead of breaking the build.
"""

import argparse
import shutil
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", default="clang-format", help="clang-format executable")
    parser.add_argument("files", nargs="+", help="files to reformat in place")
    args = parser.parse_args()

    executable = shutil.which(args.clang_format)
    if executable is None:
        print(f"clang_format: {args.clang_format} not found, nothing to do", file=sys.stderr)
        return 0

    failed = 0
    for path in args.files:
        result = subprocess.run(
            [executable, "-i", "--style=file", path],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            failed += 1
            message = result.stderr.strip() or f"exit status {result.returncode}"
            print(f"clang_format: skipped {path}: {message}", file=sys.stderr)

    if failed:
        print(f"clang_format: {failed} of {len(args.files)} file(s) left unformatted",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
