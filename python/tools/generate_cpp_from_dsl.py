#!/usr/bin/env python3

import sys
import os

# Add the parent directory (which contains the 'dsl' package) to sys.path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_PYTHON_DIR = os.path.join(SCRIPT_DIR, "..")
sys.path.insert(0, PROJECT_PYTHON_DIR)

import argparse
from pathlib import Path

from dsl.parser import Parser
from dsl.validator import Validator, ValidationError
from dsl.generator_cpp import CppGenerator


def main():
    ap = argparse.ArgumentParser(description="Generate C++17 header from DSL schema")
    ap.add_argument("input", help="Input .dsl schema file")
    ap.add_argument("output", help="Output .hpp file to write")
    ap.add_argument("--namespace", required=True,
                    help="C++ namespace for generated code (e.g. pubsub_itc_fw)")
    args = ap.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)

    text = input_path.read_text()

    try:
        ast = Parser(text).parse()
        Validator(ast).validate()
    except ValidationError as error:
        print(f"{input_path}: {error}", file=sys.stderr)
        sys.exit(1)

    gen = CppGenerator(namespace=args.namespace)
    code = gen.emit(ast)

    output_path.write_text(code)


if __name__ == "__main__":
    main()
