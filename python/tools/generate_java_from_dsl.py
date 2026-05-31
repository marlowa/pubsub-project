#!/usr/bin/env python3
"""Generate a Java source file from a pubsub_itc_fw DSL schema."""

import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_PYTHON_DIR = os.path.join(SCRIPT_DIR, "..")
sys.path.insert(0, PROJECT_PYTHON_DIR)

import argparse
from pathlib import Path

from dsl.parser import Parser
from dsl.validator import Validator, ValidationError
from dsl.generator_java import JavaGenerator


def main():
    ap = argparse.ArgumentParser(description="Generate Java source file from DSL schema")
    ap.add_argument("input", help="Input .dsl schema file")
    ap.add_argument("output", help="Output .java file to write")
    ap.add_argument(
        "--package",
        default="",
        help="Java package declaration (e.g. com.example.app). Omit for no package.",
    )
    args = ap.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    class_name = output_path.stem

    text = input_path.read_text()
    try:
        ast = Parser(text).parse()
        Validator(ast).validate()
    except ValidationError as error:
        print(f"{input_path}: {error}", file=sys.stderr)
        sys.exit(1)

    gen = JavaGenerator(class_name=class_name, package_name=args.package)
    code = gen.emit(ast)
    output_path.write_text(code)


if __name__ == "__main__":
    main()
