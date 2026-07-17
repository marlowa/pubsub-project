#!/usr/bin/env python3

"""Generate a C++17 FIX dictionary header from QuickFIX-style XML dictionaries."""

import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_PYTHON_DIR = os.path.join(SCRIPT_DIR, "..")
sys.path.insert(0, PROJECT_PYTHON_DIR)

# pylint: disable=wrong-import-position
import argparse
from pathlib import Path

from fix_dictionary import DictionaryError, emit_header, parse_dictionaries


def main():
    """Parse arguments, build the dictionary, and write the generated header."""
    parser = argparse.ArgumentParser(
        description="Generate a zero-copy C++17 FIX dictionary header (tag numbers, "
                    "message types, enumerated values, tag->name table, and DATA/LENGTH "
                    "field pairing) from one or more QuickFIX-style FIX XML dictionaries."
    )
    parser.add_argument("input", nargs="+", metavar="XML",
                        help="Input FIX XML dictionary file(s). Multiple files are merged; "
                             "for FIX 5.0 pass FIXT11.xml and FIX50SP2.xml together.")
    parser.add_argument("--output", required=True, metavar="HPP",
                        help="Output .hpp file to write.")
    parser.add_argument("--namespace", default="fix_codec",
                        help="C++ namespace for the generated code (default: fix_codec).")

    args = parser.parse_args()

    inputs = [Path(path) for path in args.input]
    try:
        dictionary = parse_dictionaries(inputs)
    except DictionaryError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)

    code = emit_header(dictionary, namespace=args.namespace)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(code, encoding="utf-8")


if __name__ == "__main__":
    main()
