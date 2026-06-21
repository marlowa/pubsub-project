#!/usr/bin/env python3

import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_PYTHON_DIR = os.path.join(SCRIPT_DIR, "..")
sys.path.insert(0, PROJECT_PYTHON_DIR)

import argparse
from pathlib import Path

from dsl.parser import Parser
from dsl.validator import Validator, ValidationError
from dsl.generator_cpp import CppGenerator
from dsl.generator_java import JavaGenerator


def main():
    ap = argparse.ArgumentParser(
        description="Generate C++ and/or Java source from a DSL schema. "
                    "At least one of --cpp or --java must be specified."
    )
    ap.add_argument("input", help="Input .dsl schema file")

    cpp_group = ap.add_argument_group("C++ output")
    cpp_group.add_argument("--cpp", metavar="OUTPUT",
                           help="Output .hpp file to write")
    cpp_group.add_argument("--namespace",
                           help="C++ namespace for generated code (required with --cpp)")
    cpp_group.add_argument("--topics", action="store_true",
                           help="Topic registry mode: enforce that a 'Topics' enum exists and "
                                "every message id references it via Topics.EntryName syntax. "
                                "Generates the Topics enum as a framework enum class.")

    java_group = ap.add_argument_group("Java output")
    java_group.add_argument("--java", metavar="OUTPUT",
                            help="Output .java file to write")
    java_group.add_argument("--package", default="",
                            help="Java package declaration (e.g. com.example.app). "
                                 "Omit for no package.")

    args = ap.parse_args()

    if not args.cpp and not args.java:
        ap.error("at least one of --cpp or --java must be specified")
    if args.cpp and not args.namespace:
        ap.error("--namespace is required when --cpp is specified")
    if args.topics and not args.cpp:
        ap.error("--topics is only valid with --cpp")
    if args.package and not args.java:
        ap.error("--package is only valid with --java")

    input_path = Path(args.input)
    text = input_path.read_text()

    try:
        ast = Parser(text).parse()
        Validator(ast).validate(topics=bool(args.topics))
    except ValidationError as error:
        print(f"{input_path}: {error}", file=sys.stderr)
        sys.exit(1)

    if args.cpp:
        gen = CppGenerator(namespace=args.namespace)
        code = gen.emit(ast, topics=args.topics)
        cpp_path = Path(args.cpp)
        cpp_path.parent.mkdir(parents=True, exist_ok=True)
        cpp_path.write_text(code)

    if args.java:
        output_path = Path(args.java)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        gen = JavaGenerator(class_name=output_path.stem, package_name=args.package)
        code = gen.emit(ast)
        output_path.write_text(code)


if __name__ == "__main__":
    main()
