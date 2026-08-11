#!/usr/bin/env python3
"""Post-build sanity check for the Doxygen architecture map.

Each box in the clickable architecture map (docs/architecture.dot) links to a
component doc via URL="\\ref <label>". If a \\ref does not resolve, Doxygen does
not always fail the build -- notably doxygen 1.8.14 on our RHEL8 hosts silently
emits href="../../" (a bare directory) into the generated image map. A browser
then opens that as a directory listing, or a file-chooser dialog on Windows,
instead of the documentation page.

This script scans the generated HTML image maps and fails if any <area> points
at a bare directory (href empty or ending in '/'), so a dead box is caught at
build time on every toolchain rather than by a reader clicking it later.

Usage:
    check_architecture_map.py [HTML_DIR]

HTML_DIR defaults to build/docs/html relative to the repository root.
"""

import re
import sys
from pathlib import Path

# Matches a full <area ...> tag and captures its href value.
area_pattern = re.compile(r"<area\b[^>]*?\bhref=\"([^\"]*)\"[^>]*>", re.IGNORECASE)
title_pattern = re.compile(r"\btitle=\"([^\"]*)\"", re.IGNORECASE)
# The architecture map's image map carries this name (derived from the dot file).
architecture_map_marker = "dot_architecture"


def find_html_dir(argv):
    if len(argv) > 1:
        return Path(argv[1])
    repository_root = Path(__file__).resolve().parent.parent
    return repository_root / "build" / "docs" / "html"


def href_is_broken(href):
    stripped = href.strip()
    return stripped == "" or stripped.endswith("/")


def main(argv):
    # Answering --help is required of every script under scripts/, and is checked by
    # build.py. No argparse here: one optional positional does not earn it.
    if any(argument in ("-h", "--help") for argument in argv[1:]):
        print(__doc__)
        return 0

    html_dir = find_html_dir(argv)
    if not html_dir.is_dir():
        print(f"check_architecture_map: HTML directory not found: {html_dir}",
              file=sys.stderr)
        return 2

    broken = []
    area_count = 0
    saw_architecture_map = False

    for html_file in sorted(html_dir.rglob("*.html")):
        text = html_file.read_text(encoding="utf-8", errors="replace")
        if architecture_map_marker in text:
            saw_architecture_map = True
        for match in area_pattern.finditer(text):
            area_count += 1
            href = match.group(1)
            if href_is_broken(href):
                title_match = title_pattern.search(match.group(0))
                title = title_match.group(1) if title_match else "(no title)"
                broken.append((html_file.relative_to(html_dir), title, href))

    if not saw_architecture_map:
        print("check_architecture_map: no architecture map found in "
              f"{html_dir} (was 'doxygen Doxyfile' run and did dot render the "
              "map?)", file=sys.stderr)
        return 2

    if broken:
        print("check_architecture_map: FAILED -- map boxes point at a bare "
              "directory (unresolved \\ref):", file=sys.stderr)
        for relative_path, title, href in broken:
            print(f"  {relative_path}: '{title}' -> href=\"{href}\"",
                  file=sys.stderr)
        print("Fix the offending URL=\"\\ref <label>\" in docs/architecture.dot "
              "and the {#label} on the target doc's heading.", file=sys.stderr)
        return 1

    print(f"check_architecture_map: OK -- {area_count} map link(s) resolve, "
          "none point at a bare directory.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
