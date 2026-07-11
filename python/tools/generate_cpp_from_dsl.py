#!/usr/bin/env python3

import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_PYTHON_DIR = os.path.join(SCRIPT_DIR, "..")
sys.path.insert(0, PROJECT_PYTHON_DIR)

import argparse
from pathlib import Path

from dsl.loader import load
from dsl.errors import DslError
from dsl.validator import Validator
from dsl.generator_cpp import CppGenerator
from dsl.generator_java import JavaGenerator
from dsl.generator_topics import TopicsGenerator
from dsl.ast import TopicDecl


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
    cpp_group.add_argument("--pdu-id-enum", action="store_true",
                           help="Pdu-id-enum mode: enforce that a 'PduId' enum exists and "
                                "every message id references it via PduId.EntryName syntax. "
                                "Generates the PduId enum as a framework enum class. (This is "
                                "the pdu-id registry, not the pub/sub topic catalog.)")

    java_group = ap.add_argument_group("Java output")
    java_group.add_argument("--java", metavar="OUTPUT",
                            help="Output .java file to write")
    java_group.add_argument("--package", default="",
                            help="Java package declaration (e.g. com.example.app). "
                                 "Omit for no package.")

    topics_group = ap.add_argument_group("Topic catalog output")
    topics_group.add_argument("--topics-registry", metavar="OUTPUT",
                              help="Output .hpp file for the generated topic registry "
                                   "(Topic enum + pdu-id/topic membership table). "
                                   "Requires --namespace.")
    topics_group.add_argument("--topics-catalog", metavar="OUTPUT",
                              help="Output .md file for the human-readable topic catalog.")

    args = ap.parse_args()

    if not (args.cpp or args.java or args.topics_registry or args.topics_catalog):
        ap.error("at least one output (--cpp, --java, --topics-registry, --topics-catalog) "
                 "must be specified")
    if args.cpp and not args.namespace:
        ap.error("--namespace is required when --cpp is specified")
    if args.topics_registry and not args.namespace:
        ap.error("--namespace is required when --topics-registry is specified")
    if args.pdu_id_enum and not args.cpp:
        ap.error("--pdu-id-enum is only valid with --cpp")
    if args.package and not args.java:
        ap.error("--package is only valid with --java")

    input_path = Path(args.input)

    try:
        ast = load(input_path)
        Validator(ast).validate(pdu_id_enum=bool(args.pdu_id_enum))
    except DslError as error:
        print(f"{input_path}: {error}", file=sys.stderr)
        sys.exit(1)

    if args.cpp:
        gen = CppGenerator(namespace=args.namespace)
        code = gen.emit(ast, pdu_id_enum=args.pdu_id_enum)
        cpp_path = Path(args.cpp)
        cpp_path.parent.mkdir(parents=True, exist_ok=True)
        cpp_path.write_text(code)

    if args.java:
        output_path = Path(args.java)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        gen = JavaGenerator(class_name=output_path.stem, package_name=args.package)
        code = gen.emit(ast)
        output_path.write_text(code)

    if args.topics_registry or args.topics_catalog:
        if not any(isinstance(decl, TopicDecl) for decl in ast.declarations):
            print(f"{input_path}: no 'topic' declarations found; nothing to generate for "
                  f"--topics-registry/--topics-catalog", file=sys.stderr)
            sys.exit(1)
        topics_gen = TopicsGenerator(namespace=args.namespace or "")
        if args.topics_registry:
            registry_path = Path(args.topics_registry)
            registry_path.parent.mkdir(parents=True, exist_ok=True)
            registry_path.write_text(topics_gen.emit_registry(ast, input_path.name))
        if args.topics_catalog:
            catalog_path = Path(args.topics_catalog)
            catalog_path.parent.mkdir(parents=True, exist_ok=True)
            catalog_path.write_text(topics_gen.emit_catalog(ast, input_path.name))


if __name__ == "__main__":
    main()
