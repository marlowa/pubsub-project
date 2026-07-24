"""Command-line entry point: generate (or check) a .dsl from a FIX DD + a message list.

The whole "what to generate" instruction lives on the command line -- the data
dictionary and the messages (with their PDU ids) -- so there is no separate field-list
spec. The DD is the source of truth for fields, types, enums, order and repeating
groups; each named message is expanded in full.

    # spec-less (preferred): DD + message list on the command line
    python -m dd_to_dsl --dd venue.xml \
        --message NewOrderSingle:1000 --message OrderCancelRequest:1001 \
        --message ExecutionReport:1002 --output build/fix_equity_orders.dsl

    python -m dd_to_dsl --dd venue.xml --all --output build/orders.dsl   # every message in the DD
    python -m dd_to_dsl --dd venue.xml --all --stdout                    # print, do not write

    # legacy: a TOML selection spec (its field lists are ignored; only its message
    # names + PDU ids and data_dictionary are used)
    python -m dd_to_dsl spec.toml [--check|--stdout]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from fix_dictionary.parser import parse_dictionaries

from dd_to_dsl.generator import GeneratorError, generate_dsl
from dd_to_dsl.spec import MessageSpec, Spec, SpecError, load_spec


def _parse_message_arg(text: str) -> MessageSpec:
    """Parse a ``Name:id`` command-line message argument into a MessageSpec."""
    name, separator, id_text = text.partition(":")
    if not name or not separator or not id_text:
        raise GeneratorError(f"--message must be NAME:ID (got '{text}')")
    try:
        pdu_id = int(id_text)
    except ValueError as error:
        raise GeneratorError(f"--message '{text}': id '{id_text}' is not an integer") from error
    return MessageSpec(name=name, pdu_id=pdu_id)


def _cli_spec(args, root: Path) -> Spec:
    """Build a Spec from the spec-less command-line arguments (--dd / --message / --all)."""
    if not args.dd:
        raise GeneratorError("--dd is required (one or more FIX data dictionary files)")
    if not args.output and not args.stdout:
        raise GeneratorError("--output is required (or use --stdout)")
    if args.all and args.message:
        raise GeneratorError("give either --all or explicit --message entries, not both")
    if not args.all and not args.message:
        raise GeneratorError("specify the messages to generate: --message NAME:ID ... or --all")

    data_dictionary = [str(path) for path in args.dd]
    if args.all:
        # Every message in the DD, in DD declaration order, with ids from --id-base.
        dictionary = parse_dictionaries([root / path for path in data_dictionary])
        messages = [MessageSpec(name=message_def.name, pdu_id=args.id_base + index) for index, message_def in enumerate(dictionary.messages.values())]
        if not messages:
            raise GeneratorError("--all: the data dictionary defines no messages")
    else:
        messages = [_parse_message_arg(entry) for entry in args.message]

    return Spec(output=str(args.output) if args.output else "", data_dictionary=data_dictionary, enums=[], messages=messages)


def main(argv=None) -> int:
    """Parse arguments, generate the DSL, and write/check it; return a process exit code."""
    parser = argparse.ArgumentParser(prog="dd_to_dsl", description="Generate a serialisation .dsl by expanding FIX messages from a data dictionary.")
    parser.add_argument("spec", type=Path, nargs="?", help="legacy: a TOML selection spec (mutually exclusive with --dd)")
    parser.add_argument("--dd", type=Path, action="append", default=[], metavar="XML", help="FIX data dictionary file (repeatable; merged in order)")
    parser.add_argument("--message", action="append", default=[], metavar="NAME:ID", help="a message to generate and its PDU id (repeatable)")
    parser.add_argument("--all", action="store_true", help="generate every message in the data dictionary (ids assigned from --id-base)")
    parser.add_argument("--id-base", type=int, default=1000, help="first PDU id assigned by --all, in DD order (default: 1000)")
    parser.add_argument("--output", type=Path, help="output .dsl file (spec-less mode; legacy mode uses the spec's output)")
    parser.add_argument("--root", type=Path, default=None, help="project root for resolving relative paths (default: current directory)")
    parser.add_argument("--check", action="store_true", help="do not write; exit non-zero if the output file is out of date")
    parser.add_argument("--stdout", action="store_true", help="write the generated DSL to stdout instead of the output file")
    args = parser.parse_args(argv)

    if args.spec and (args.dd or args.message or args.all):
        parser.error("give either a spec file or the --dd/--message flags, not both")
    if not args.spec and not args.dd:
        parser.error("provide a spec file, or --dd with --message/--all")

    root = (args.root or Path.cwd()).resolve()
    try:
        spec = load_spec(args.spec) if args.spec else _cli_spec(args, root)
        text = generate_dsl(spec, root)
    except (SpecError, GeneratorError, OSError) as error:
        print(f"dd_to_dsl: {error}", file=sys.stderr)
        return 1

    if args.stdout:
        sys.stdout.write(text)
        return 0

    output_path = root / spec.output
    if args.check:
        current = output_path.read_text(encoding="utf-8") if output_path.exists() else ""
        if current != text:
            print(f"dd_to_dsl: {spec.output} is out of date; regenerate it", file=sys.stderr)
            return 1
        return 0

    output_path.write_text(text, encoding="utf-8")
    print(f"dd_to_dsl: wrote {spec.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
