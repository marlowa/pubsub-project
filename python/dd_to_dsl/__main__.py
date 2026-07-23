"""Command-line entry point: generate (or check) a .dsl from a FIX DD + selection spec.

    python -m dd_to_dsl <spec.toml>            # write spec.output
    python -m dd_to_dsl <spec.toml> --check    # fail if spec.output is stale (CI guard)
    python -m dd_to_dsl <spec.toml> --stdout    # print, do not write
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from dd_to_dsl.generator import GeneratorError, generate_dsl
from dd_to_dsl.spec import SpecError, load_spec


def main(argv=None) -> int:
    """Parse arguments, generate the DSL, and write/check it; return a process exit code."""
    parser = argparse.ArgumentParser(prog="dd_to_dsl", description="Generate a serialisation .dsl from a FIX data dictionary and a selection spec.")
    parser.add_argument("spec", type=Path, help="path to the TOML selection spec")
    parser.add_argument("--root", type=Path, default=None, help="project root for resolving the spec's relative paths (default: current directory)")
    parser.add_argument("--check", action="store_true", help="do not write; exit non-zero if the output file is out of date")
    parser.add_argument("--stdout", action="store_true", help="write the generated DSL to stdout instead of the output file")
    args = parser.parse_args(argv)

    root = (args.root or Path.cwd()).resolve()
    try:
        spec = load_spec(args.spec)
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
            print(f"dd_to_dsl: {spec.output} is out of date; regenerate with 'python -m dd_to_dsl {args.spec}'", file=sys.stderr)
            return 1
        return 0

    output_path.write_text(text, encoding="utf-8")
    print(f"dd_to_dsl: wrote {spec.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
