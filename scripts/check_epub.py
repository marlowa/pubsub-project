#!/usr/bin/env python3
"""check_epub.py -- whether an EPUB the book's build wrote is sound enough for a reader to open.

    python3 scripts/check_epub.py docs/book/book.epub

tex4ebook reports errors during every conversion of the book, because TeX4ht's support for the glossaries
package typesets a stray word before the document begins (docs/book/book.mk4 says which, and removes it), so
whether the conversion succeeded cannot be read from its output. This reads the EPUB itself instead, and
checks what an e-book reader depends on:

- the file is a zip archive whose first entry is "mimetype", stored without compression and holding
  "application/epub+zip", which is how a reader recognises an EPUB;
- every page, and the package and navigation files, parse as XML, which EPUB 3 requires of them and which
  the stray word breaks if book.mk4 fails to remove it;
- no page still holds the stray word.

It exits 0 when all of that holds, 1 when anything does not, naming each file and what is wrong, and 2
when it is not given a file it can open.
"""

from __future__ import annotations

import sys
import zipfile
import xml.dom.minidom
import xml.parsers.expat

# The word TeX4ht's glossary support writes before \begin{document}; see docs/book/book.mk4.
_STRAY_WORD = b"refextract"
_XML_SUFFIXES = (".xhtml", ".html", ".opf", ".ncx", ".xml")


def problems_in(epub_path: str) -> list[str]:
    """Everything wrong with the EPUB at epub_path, or an empty list."""
    problems: list[str] = []
    with zipfile.ZipFile(epub_path) as archive:
        entries = archive.infolist()
        if not entries or entries[0].filename != "mimetype":
            problems.append("the first entry is not 'mimetype', so a reader will not recognise the file as an EPUB")
        else:
            first = entries[0]
            if first.compress_type != zipfile.ZIP_STORED:
                problems.append("'mimetype' is compressed; it must be stored as it is")
            if archive.read(first).strip() != b"application/epub+zip":
                problems.append(f"'mimetype' holds {archive.read(first)!r}, not 'application/epub+zip'")
        checked = 0
        for entry in entries:
            if not entry.filename.endswith(_XML_SUFFIXES):
                continue
            checked += 1
            data = archive.read(entry)
            if _STRAY_WORD in data:
                problems.append(f"{entry.filename} still holds the stray word '{_STRAY_WORD.decode()}' that book.mk4 removes")
            try:
                xml.dom.minidom.parseString(data)
            except xml.parsers.expat.ExpatError as error:
                problems.append(f"{entry.filename} is not well-formed XML: {error}")
        if checked == 0:
            problems.append("it holds no pages")
    return problems


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_epub.py <book.epub>", file=sys.stderr)
        return 2
    epub_path = sys.argv[1]
    try:
        problems = problems_in(epub_path)
    except (OSError, zipfile.BadZipFile) as error:
        print(f"check_epub.py: cannot read {epub_path}: {error}", file=sys.stderr)
        return 2
    if problems:
        print(f"{epub_path} is not sound:")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print(f"{epub_path} is sound: a valid EPUB container, and every page parses as XML")
    return 0


if __name__ == "__main__":
    sys.exit(main())
