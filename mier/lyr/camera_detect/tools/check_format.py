#!/usr/bin/env python3
"""Check or format owned A35 C/C++ sources with a pinned clang-format."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


PROJECT_ROOT = Path(__file__).resolve().parents[1]
FORMAT_VERSION = "18.1.8"


def source_files() -> list[Path]:
    """Include application files, excluding ST headers and upstream cJSON."""
    files = []
    for directory in (PROJECT_ROOT, PROJECT_ROOT / "hud_project"):
        for path in directory.iterdir():
            if path.suffix in {".c", ".h", ".cpp", ".hpp"} and path.stem != "cJSON":
                files.append(path)
    return sorted(files)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="format files in place")
    parser.add_argument("--clang-format", default="clang-format", help="executable path")
    args = parser.parse_args()
    try:
        version = subprocess.run(
            [args.clang_format, "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        if re.search(r"\bversion " + re.escape(FORMAT_VERSION) + r"\b", version) is None:
            print(f"Required clang-format {FORMAT_VERSION}; found: {version.strip()}", file=sys.stderr)
            return 2
        files = source_files()
        if not files:
            print("No application sources found", file=sys.stderr)
            return 2
        options = ["-i"] if args.fix else ["--dry-run", "--Werror"]
        result = subprocess.run(
            [args.clang_format, "--style=file", *options, *(str(path) for path in files)],
            cwd=PROJECT_ROOT,
            check=False,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Cannot run clang-format: {error}", file=sys.stderr)
        return 2
    if result.returncode == 0:
        action = "Formatted" if args.fix else "Checked"
        print(f"{action} {len(files)} application source files with clang-format {FORMAT_VERSION}")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
