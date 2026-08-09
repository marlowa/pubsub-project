#!/usr/bin/env python3
"""
check_standards.py -- Check C++ source files for coding-standard violations.

Checks implemented:
  1.  #define outside LoggingMacros.hpp
  2.  SCREAMING_SNAKE_CASE identifiers in non-macro contexts
  3.  camelCase identifiers (variables/functions must be snake_case)
  4.  NULL instead of nullptr
  5.  std::size_t instead of size_t
  6.  using namespace std
  7.  noexcept keyword
  8.  final keyword on classes or methods
  9.  #include with double quotes. Banned in library code (no exceptions). In application code,
      double quotes are allowed only for the cpp's own same-directory headers (bare filename, no '/');
      every other include still uses angle brackets. tests/integration_tests/performance exempted.
  10. Lines exceeding 160 characters
  11. Non-ASCII characters
  12. End-of-brace comments (} // ...), except '} // namespaces' and '} // un-named namespace'
  13. Double underscore in user-defined identifiers
  14. Trailing return type syntax (auto f() ->)
  15. Tabs used for indentation
  16. East const style (T const* / T const&)
  17. static function at file scope in .cpp (prefer unnamed namespace)
  18. s_ prefix on class static members
  19. @throws in Doxygen comments (pubsub_itc_fw only)
  20. Missing #pragma once in .hpp files
  21. Template keyword on its own line before <
  22. Banner / divider comment lines (rows of - or =)
  23. #include ordering: external / third-party headers before project headers
  24. Single-argument constructor not declared explicit
  25. Bare true / false literal passed as a function argument
  26. printf family (printf/sprintf/snprintf/fprintf and v- variants); use fmt
"""

from __future__ import annotations
import argparse
import bisect
import re
import sys
from pathlib import Path

# ── Directories skipped during recursive discovery ────────────────────────────

_EXCLUDE_DIRS = frozenset({
    'build', 'installed', 'cmake-build-debug', 'cmake-build-release',
    '.git', 'thirdparty', '__pycache__',
})

# ── Violation ─────────────────────────────────────────────────────────────────

class Violation:
    def __init__(self, path: Path, line_number: int, message: str) -> None:
        self.path = path
        self.line_number = line_number
        self.message = message

    def __str__(self) -> str:
        return f"{self.path}:{self.line_number}: {self.message}"


# ── Comment and string stripping ──────────────────────────────────────────────

def strip_comments_and_strings(text: str) -> str:
    """
    Replace the interior of C++ comments and string/char literals with spaces,
    preserving newlines so that line numbers remain correct.
    """
    result = []
    i = 0
    n = len(text)
    while i < n:
        if text[i:i+2] == '/*':
            result.append('/*')
            i += 2
            while i < n and text[i:i+2] != '*/':
                result.append(' ' if text[i] != '\n' else '\n')
                i += 1
            if i < n:
                result.append('*/')
                i += 2
        elif text[i:i+2] == '//':
            result.append('//')
            i += 2
            while i < n and text[i] != '\n':
                result.append(' ')
                i += 1
        elif text[i] == '"':
            result.append('"')
            i += 1
            while i < n and text[i] not in ('"', '\n'):
                if text[i] == '\\' and i + 1 < n:
                    result.append('  ')
                    i += 2
                else:
                    result.append(' ')
                    i += 1
            if i < n and text[i] == '"':
                result.append('"')
                i += 1
        elif text[i] == "'":
            result.append("'")
            i += 1
            while i < n and text[i] not in ("'", '\n'):
                if text[i] == '\\' and i + 1 < n:
                    result.append('  ')
                    i += 2
                else:
                    result.append(' ')
                    i += 1
            if i < n and text[i] == "'":
                result.append("'")
                i += 1
        else:
            result.append(text[i])
            i += 1
    return ''.join(result)


# ── Helpers ───────────────────────────────────────────────────────────────────

_DEFINE_RE = re.compile(r'^\s*#\s*define\b')


def _is_define_line(line: str) -> bool:
    return bool(_DEFINE_RE.match(line))


# ── Check 1: #define outside LoggingMacros.hpp ───────────────────────────────
# Defines inside #ifdef CLANG_TIDY blocks are exempt: they are system-level
# feature-test macros or test-infrastructure workarounds that cannot be
# replaced with constexpr.

_IFDEF_ANY_RE = re.compile(r'^\s*#\s*if(?:def|ndef)?\b')
_IFDEF_CLANG_TIDY_RE = re.compile(r'^\s*#\s*ifdef\s+CLANG_TIDY\b')
_ENDIF_RE = re.compile(r'^\s*#\s*endif\b')

def check_defines(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if path.name == 'LoggingMacros.hpp':
        return []
    violations = []
    ifdef_stack: list[bool] = []  # True = this nesting level is a CLANG_TIDY block
    for i, (line, sline) in enumerate(zip(lines, stripped), 1):
        raw = line.strip()
        if _IFDEF_CLANG_TIDY_RE.match(raw):
            ifdef_stack.append(True)
        elif _IFDEF_ANY_RE.match(raw):
            ifdef_stack.append(False)
        elif _ENDIF_RE.match(raw) and ifdef_stack:
            ifdef_stack.pop()
        if any(ifdef_stack):
            continue
        if _DEFINE_RE.match(sline):
            violations.append(Violation(path, i,
                '#define used outside LoggingMacros.hpp; use constexpr or inline instead'))
    return violations


# ── Check 2: SCREAMING_SNAKE_CASE in non-macro contexts ──────────────────────
# Extracts only the *declared* identifier from a constexpr declaration so that
# SCREAMING names used on the right-hand side (e.g. system constants such as
# INET6_ADDRSTRLEN) are not falsely flagged.

_SCREAMING_DECL_EXTRACT_RE = re.compile(
    r'\b(?:static\s+)?constexpr\s+'   # constexpr keyword
    r'[\w:<>*&\s]+?\s+'               # type (non-greedy)
    r'([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)'  # SCREAMING declared name
    r'\s*[=;{\[]'                     # followed by =, ;, {, or [
)

def check_screaming_snake_case(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _is_define_line(line):
            continue
        match = _SCREAMING_DECL_EXTRACT_RE.search(line)
        if match:
            name = match.group(1)
            violations.append(Violation(path, i,
                f"SCREAMING_SNAKE_CASE constant '{name}' -- "
                f"only macros may use SCREAMING_SNAKE_CASE; use snake_case for constants"))
    return violations


# ── Check 3: camelCase identifiers ───────────────────────────────────────────
# Variables, parameters, and function names must be snake_case.
# PascalCase (class/struct/type names) is correct and is not flagged here.

_CAMEL_CASE_RE = re.compile(r'\b([a-z][a-z0-9]*[A-Z][a-zA-Z0-9]*)\b')

# Known external/standard identifiers that legitimately use camelCase.
_CAMEL_CASE_ALLOWED = frozenset({
    'nullptr', 'noexcept', 'constexpr', 'sizeof', 'alignof', 'decltype',
    'reinterpret_cast', 'static_cast', 'dynamic_cast', 'const_cast',
    'typeid', 'true', 'false',
    # QuickFIX/J internal calls visible in Java but not in C++; none expected.
    # Add external C++ API names here if false positives arise.
})

def check_camel_case(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    seen_on_line: set[int] = set()
    for i, line in enumerate(stripped, 1):
        if _is_define_line(line):
            continue
        if line.lstrip().startswith('#include'):
            continue
        for match in _CAMEL_CASE_RE.finditer(line):
            name = match.group(1)
            if name in _CAMEL_CASE_ALLOWED:
                continue
            if i not in seen_on_line:
                violations.append(Violation(path, i,
                    f"camelCase identifier '{name}' -- use snake_case for variables and functions"))
                seen_on_line.add(i)
    return violations


# ── Check 4: NULL instead of nullptr ─────────────────────────────────────────

_NULL_RE = re.compile(r'\bNULL\b')

def check_null(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _NULL_RE.search(line):
            violations.append(Violation(path, i, "NULL used; use nullptr instead"))
    return violations


# ── Check 5: std::size_t instead of size_t ───────────────────────────────────

_STD_SIZE_T_RE = re.compile(r'\bstd::size_t\b')

def check_std_size_t(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _STD_SIZE_T_RE.search(line):
            violations.append(Violation(path, i,
                "std::size_t used; write size_t instead (the typedef needs no qualification)"))
    return violations


# ── Check 6: using namespace std ─────────────────────────────────────────────

_USING_NS_STD_RE = re.compile(r'\busing\s+namespace\s+std\b')

def check_using_namespace_std(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _USING_NS_STD_RE.search(line):
            violations.append(Violation(path, i, "'using namespace std' is banned"))
    return violations


# ── Check 7: noexcept ─────────────────────────────────────────────────────────

_NOEXCEPT_RE = re.compile(r'\bnoexcept\b')

def check_noexcept(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _NOEXCEPT_RE.search(line):
            violations.append(Violation(path, i,
                "'noexcept' is banned (coding rules: do not use noexcept, not even on dtors)"))
    return violations


# ── Check 8: final keyword ────────────────────────────────────────────────────

_FINAL_CLASS_RE = re.compile(r'\b(?:class|struct)\s+\w+\s+final\b')
_FINAL_METHOD_RE = re.compile(r'\)\s*(?:const\s+)?final\b')

def check_final(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _FINAL_CLASS_RE.search(line) or _FINAL_METHOD_RE.search(line):
            violations.append(Violation(path, i,
                "'final' keyword is banned (coding rules: do not use final)"))
    return violations


# ── Check 9: #include with double quotes ─────────────────────────────────────
# The rule: includes use angle brackets, with one exception. Library code has no
# exceptions -- every include uses angle brackets. Application code (under
# 'applications/') uses double quotes only for the headers that sit alongside the
# cpp (its own local headers, bare filename, no '/'); every other include, including
# any header referenced by a path, still uses angle brackets. A double-quoted include
# that contains a '/' is a cross-library reference and is always a violation.
# Exempted entirely: test, integration-test, and performance directories.

_INCLUDE_QUOTE_RE = re.compile(r'^\s*#\s*include\s+"([^"]*)"')
_INCLUDE_QUOTE_EXEMPT_DIRS = frozenset({'tests', 'integration_tests', 'performance'})

def check_include_quotes(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if any(part in _INCLUDE_QUOTE_EXEMPT_DIRS for part in path.parts):
        return []
    is_application = 'applications' in path.parts
    violations = []
    for i, line in enumerate(lines, 1):
        m = _INCLUDE_QUOTE_RE.match(line)
        if m:
            included = m.group(1)
            if is_application and '/' not in included:
                continue  # local app header -- double quotes are fine
            violations.append(Violation(path, i,
                '#include uses double quotes; all includes must use <angle brackets>'))
    return violations


# ── Check 10: lines exceeding 160 characters ─────────────────────────────────

def check_line_length(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        length = len(line.rstrip('\n\r'))
        if length > 160:
            violations.append(Violation(path, i,
                f"line length {length} exceeds 160-character limit"))
    return violations


# ── Check 11: non-ASCII characters ───────────────────────────────────────────

def check_non_ascii(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        for col, ch in enumerate(line, 1):
            if ord(ch) > 127:
                violations.append(Violation(path, i,
                    f"non-ASCII character U+{ord(ch):04X} at column {col} "
                    f"(source files must contain only ASCII)"))
                break
    return violations


# ── Check 12: end-of-brace comments ─────────────────────────────────────────
# Two permitted forms only:
#   '} // namespaces'         — closes one or more named namespaces (no name
#                               repeated, so nothing rots when namespaces are renamed)
#   '} // un-named namespace' — closes an unnamed namespace

_BRACE_COMMENT_RE = re.compile(r'\}\s*//')
_BRACE_COMMENT_ALLOWED_RE = re.compile(
    r'\}\s*//\s*(?:namespaces|un-named namespace)\s*$'
)
_BRACE_COMMENT_NAMED_NS_RE = re.compile(r'\}\s*//\s*namespace\b')
_BRACE_COMMENT_UNNAMED_NS_RE = re.compile(r'\}\s*//\s*(?:unnamed|anonymous)\s*namespace\b')

def check_brace_comments(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        if _BRACE_COMMENT_RE.search(line) and not _BRACE_COMMENT_ALLOWED_RE.search(line):
            if _BRACE_COMMENT_UNNAMED_NS_RE.search(line):
                msg = "unnamed namespace closing brace must use '} // un-named namespace'"
            elif _BRACE_COMMENT_NAMED_NS_RE.search(line):
                msg = ("named namespace closing brace must use '} // namespaces' "
                       "(do not repeat the namespace name)")
            else:
                msg = "end-of-brace comment is banned"
            violations.append(Violation(path, i, msg))
    return violations


# ── Check 13: double underscore in user-defined identifiers ──────────────────
# The C++ standard reserves all identifiers containing __.
# Predefined compiler macros starting with __ are permitted for use but not creation.

_DUNDER_IN_WORD_RE = re.compile(r'\b(\w+__\w+)\b')  # __ not at start of token
_DUNDER_PREDEFINED = frozenset({
    '__FILE__', '__LINE__', '__func__', '__FUNCTION__', '__PRETTY_FUNCTION__',
    '__cplusplus', '__GNUC__', '__GNUC_MINOR__', '__GNUC_PATCHLEVEL__',
    '__clang__', '__clang_major__', '__clang_minor__', '__clang_patchlevel__',
    '__DATE__', '__TIME__', '__STDC__', '__STDC_VERSION__',
    '__has_include', '__has_attribute', '__has_builtin', '__has_cpp_attribute',
    '__attribute__', '__builtin_expect', '__builtin_unreachable',
    '__asm__', '__volatile__', '__extension__', '__restrict',
    '__int128',
})

def check_double_underscore(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        for match in _DUNDER_IN_WORD_RE.finditer(line):
            name = match.group(1)
            if name not in _DUNDER_PREDEFINED:
                violations.append(Violation(path, i,
                    f"double underscore in identifier '{name}' "
                    f"(reserved by the C++ standard; do not create such identifiers)"))
                break
    return violations


# ── Check 14: trailing return type syntax ────────────────────────────────────

_TRAILING_RETURN_RE = re.compile(
    r'\bauto\b[^;{(=\n]*\)\s*(?:const\s*)?(?:override\s*)?->'
)

def check_trailing_return_type(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _TRAILING_RETURN_RE.search(line):
            violations.append(Violation(path, i,
                "trailing return type (auto f() ->) is banned; use traditional T f() form"))
    return violations


# ── Check 15: tabs ───────────────────────────────────────────────────────────

def check_tabs(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        if '\t' in line:
            violations.append(Violation(path, i,
                "tab character used; use spaces for indentation"))
    return violations


# ── Check 16: east const (T const* / T const&) ───────────────────────────────
# West const is preferred: const T* / const T&.
# Trailing method const (void f() const) is not a violation.

_TRAILING_METHOD_CONST_RE = re.compile(r'\)\s*const\s*(?:override\s*)?(?:=\s*0\s*)?[;{]')
_EAST_CONST_RE = re.compile(r'\b(\w[\w<>:]*)\s+const\s*([*&])')

def check_east_const(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        cleaned = _TRAILING_METHOD_CONST_RE.sub(') ', line)
        for match in _EAST_CONST_RE.finditer(cleaned):
            type_name = match.group(1)
            ptr_or_ref = match.group(2)
            if type_name == 'const':
                continue
            violations.append(Violation(path, i,
                f"east const: write 'const {type_name}{ptr_or_ref}', "
                f"not '{type_name} const{ptr_or_ref}'"))
            break
    return violations


# ── Check 17: static function at file scope in .cpp ──────────────────────────
# Prefer an unnamed namespace. Only checked in .cpp files.

_STATIC_FUNC_RE = re.compile(
    r'^static\s+(?!(?:constexpr|const|inline|thread_local|assert)\b)'
    r'(?:const\s+)?(\w[\w<>:]*)\s+\w+\s*\('
)

def check_static_file_scope(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if path.suffix != '.cpp':
        return []
    violations = []
    for i, line in enumerate(stripped, 1):
        if _STATIC_FUNC_RE.match(line):
            violations.append(Violation(path, i,
                "static file-scope function; "
                "prefer an unnamed namespace (namespace { ... })"))
    return violations


# ── Check 18: s_ prefix on class static members ──────────────────────────────

_S_PREFIX_RE = re.compile(r'\bstatic\s+[\w:<>]+\s+s_\w+')

def check_s_prefix_static(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _S_PREFIX_RE.search(line):
            violations.append(Violation(path, i,
                "s_ prefix on static member is banned "
                "(coding rules: do not use s_ prefix for class static members)"))
    return violations


# ── Check 19: @throws in Doxygen comments (pubsub_itc_fw only) ───────────────
# Other libraries (e.g. scram_crypto) may use std::runtime_error and document
# it with @throws. The convention only applies to pubsub_itc_fw.

_DOXYGEN_THROWS_RE = re.compile(r'@throws?\b')

def check_doxygen_throws(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if 'pubsub_itc_fw' not in path.parts:
        return []
    violations = []
    for i, line in enumerate(lines, 1):
        if _DOXYGEN_THROWS_RE.search(line):
            violations.append(Violation(path, i,
                "@throws/@throw in Doxygen comment is banned (coding rules)"))
    return violations


# ── Check 20: missing #pragma once in .hpp files ─────────────────────────────

_PRAGMA_ONCE_RE = re.compile(r'^\s*#\s*pragma\s+once\b', re.MULTILINE)

def check_pragma_once(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if path.suffix != '.hpp':
        return []
    full_text = ''.join(lines)
    if not _PRAGMA_ONCE_RE.search(full_text):
        return [Violation(path, 1, "#pragma once missing from header file")]
    return []


# ── Check 21: template keyword on its own line before < ─────────────────────

_TEMPLATE_SPLIT_RE = re.compile(r'\btemplate\s*$')

def check_template_on_own_line(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _TEMPLATE_SPLIT_RE.search(line.rstrip()):
            violations.append(Violation(path, i,
                "template keyword on its own line; "
                "the parameter list must be on the same line: template <...>"))
    return violations


# ── Check 22: banner / divider comment lines ────────────────────────────────

# A comment whose entire body is a run of - or = (optionally spaced): a decorative
# divider. Banned by the coding rules ("no banner blocks / divider lines").
_BANNER_DIVIDER_RE = re.compile(r'^\s*//\s*[-=]{4,}\s*$')

def check_banner_dividers(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        if _BANNER_DIVIDER_RE.match(line):
            violations.append(Violation(path, i,
                "banner / divider comment (row of - or =); delete it -- comments must not "
                "decorate or restate the code (see coding rules: no banner blocks)"))
    return violations


# ── Check 23: #include ordering (external before project) ────────────────────
# The coding rule groups includes as: external headers (C++ standard, C / POSIX
# / third-party), then the header matching the cpp, then other project headers.
# This check enforces the coarse, unambiguous part every file follows: all
# external headers must precede the project headers. It does not police the finer
# sub-order (own header first, project headers alphabetical), which the codebase
# does not apply uniformly and which would produce false positives.
#
# Classification: a project header is an angle include ending in .hpp whose top
# path element is not a third-party library shipped as .hpp. Every other angle
# include is external -- extension-less C++ standard headers, anything ending in
# .h (C / POSIX / openssl / quill / fmt), and the third-party .hpp libraries.
# Application-local headers are double-quoted (permitted by check 9), so they are
# not matched here and their own convention is untouched.
#
# Two orderings are accepted for a .cpp: external headers first then project
# headers, or the cpp's own matching header first (self-containment check) then
# external then other project headers. A leading own header is therefore skipped.
# For a FooTest.cpp the self-containment header is the header under test (Foo.hpp),
# so that is skipped too. The failure this catches is external headers split
# around the project headers, e.g. an external header left after a project header.

_INCLUDE_ANGLE_RE = re.compile(r'^\s*#\s*include\s+<([^>]*)>')
_NAMESPACE_START_RE = re.compile(r'^\s*namespace\b')
_THIRD_PARTY_HPP_ROOTS = frozenset({'toml++', 'argparse'})

def _include_is_project_header(included: str) -> bool:
    base = included.rsplit('/', 1)[-1]
    if not base.endswith('.hpp'):
        return False
    top = included.split('/', 1)[0]
    return top not in _THIRD_PARTY_HPP_ROOTS

def _leading_own_headers(path: Path) -> frozenset:
    if path.suffix != '.cpp':
        return frozenset()
    stem = path.stem
    headers = {stem + '.hpp'}
    if stem.endswith('Test'):
        headers.add(stem[:-len('Test')] + '.hpp')  # a FooTest.cpp may lead with the header under test
    return frozenset(headers)

def check_include_order(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    own_headers = _leading_own_headers(path)
    violations = []
    seen_any_include = False
    first_project_line = 0
    first_project_name = ''
    for i, line in enumerate(lines, 1):
        if _NAMESPACE_START_RE.match(line):
            break  # leading include region ends; later includes are conditional / mid-file
        match = _INCLUDE_ANGLE_RE.match(line)
        if match is None:
            continue
        included = match.group(1)
        is_own = included.rsplit('/', 1)[-1] in own_headers
        if not seen_any_include and is_own:
            seen_any_include = True  # leading own header is allowed to precede the external block
            continue
        seen_any_include = True
        if _include_is_project_header(included):
            if first_project_line == 0:
                first_project_line = i
                first_project_name = included
        elif first_project_line != 0:
            violations.append(Violation(path, i,
                f'#include <{included}> (an external header) is out of order; every system, standard, '
                f'and third-party header must precede the project headers, but this one follows '
                f'<{first_project_name}> included at line {first_project_line}'))
    return violations


# ── Check 24: single-argument constructor not declared explicit ──────────────
# A constructor callable with exactly one argument must be explicit so it cannot
# act as an implicit conversion. Copy and move constructors (single parameter of
# the class's own type taken by reference) are exempt: they convert nothing and
# must not be explicit. Defaulted and deleted constructors are skipped. Only
# in-class declarations are inspected; an out-of-line definition (Type::Type(...))
# never carries the explicit keyword and so never matches.

# A deliberately implicit converting constructor opts out of the explicit rule by
# carrying this marker on its declaration line or the line directly above it.
_IMPLICIT_CTOR_OK = 'implicit-ctor-ok'

# An optional alignas(...) specifier may sit between the class/struct keyword and
# the type name (struct alignas(64) Slot { ... }); skip it so the name is captured,
# not the specifier.
_CLASS_NAME_RE = re.compile(r'\b(enum\s+)?(?:class|struct)\s+(?:alignas\s*\([^)]*\)\s*)?(\w+)')

def _constructible_type_names(stripped_text: str) -> set:
    names = set()
    for match in _CLASS_NAME_RE.finditer(stripped_text):
        if match.group(1) is not None:
            continue  # enum class / enum struct is not a constructible type
        names.add(match.group(2))
    return names

def _has_top_level_comma(parameters: str) -> bool:
    depth = 0
    for character in parameters:
        if character in '([{<':
            depth += 1
        elif character in ')]}>':
            depth -= 1
        elif character == ',' and depth == 0:
            return True
    return False

def check_explicit_single_arg_ctor(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    stripped_text = ''.join(stripped)
    type_names = _constructible_type_names(stripped_text)
    if not type_names:
        return []

    line_starts = []
    offset = 0
    for line in stripped:
        line_starts.append(offset)
        offset += len(line)

    names_pattern = '|'.join(re.escape(name) for name in sorted(type_names, key=len, reverse=True))
    ctor_re = re.compile(
        r'(?m)^[ \t]*(?:template\s*<[^;{}]*>\s*)?'
        r'((?:(?:explicit|constexpr|consteval|inline)\s+)*)'
        r'(' + names_pattern + r')\s*\(')

    violations = []
    for match in ctor_re.finditer(stripped_text):
        specifiers = match.group(1)
        if 'explicit' in specifiers:
            continue  # already explicit
        type_name = match.group(2)
        open_paren = match.end() - 1
        depth = 0
        close_paren = -1
        for index in range(open_paren, len(stripped_text)):
            character = stripped_text[index]
            if character == '(':
                depth += 1
            elif character == ')':
                depth -= 1
                if depth == 0:
                    close_paren = index
                    break
        if close_paren < 0:
            continue

        parameters = stripped_text[open_paren + 1:close_paren]
        if parameters.strip() == '' or _has_top_level_comma(parameters):
            continue  # default constructor or more than one parameter

        tail = stripped_text[close_paren + 1:close_paren + 40]
        if re.match(r'\s*=\s*(default|delete)', tail):
            continue
        if re.match(r'\s*->', tail):
            continue  # class template argument deduction guide, not a constructor

        is_copy_or_move = re.search(r'\b' + re.escape(type_name) + r'\b', parameters) is not None and '&' in parameters
        if is_copy_or_move:
            continue

        line_number = bisect.bisect_right(line_starts, match.start())
        if _IMPLICIT_CTOR_OK in lines[line_number - 1] or (line_number >= 2 and _IMPLICIT_CTOR_OK in lines[line_number - 2]):
            continue  # opted out: a deliberately implicit converting constructor

        violations.append(Violation(path, line_number,
            "single-argument constructor is not declared explicit; add explicit "
            "to prevent unintended implicit conversions (copy / move constructors are exempt). "
            f"If the implicit conversion is deliberate, mark it with a {_IMPLICIT_CTOR_OK} comment"))
    return violations


# ── Check 25: bare bool literal passed to a project function ──────────────────
# The coding rules ban passing a bare true / false at a call site when the callee
# is a project function that declares a bool parameter: the reader cannot tell
# what the flag means without opening the signature. Use a typed flag class (see
# UseHugePagesFlag) instead. The check fires only when the callee is a function
# defined in this project whose signature takes a bool -- so standard/library
# calls (an atomic store, make_tuple, a third-party setter) are never flagged,
# because they are not project functions and are not in the collected set. A
# literal counts only when it is a whole argument, delimited by ( or , on the
# left and ) or , on the right, so operands such as (x == true) are left alone. A
# bool return, local, or struct field is fine -- only flag arguments are policed.
# Test, integration-test and performance code is exempt. A genuinely unavoidable
# case can opt out with a bool-arg-ok comment on the call line.

_BOOL_LITERAL_RE = re.compile(r'\b(?:true|false)\b')
_BOOL_ARG_OK = 'bool-arg-ok'
_BOOL_LITERAL_EXEMPT_DIRS = frozenset({'tests', 'integration_tests', 'performance', 'tests_common'})
_NON_CALL_CALLEES = frozenset({
    'if', 'while', 'for', 'switch', 'catch', 'return',
    'assert', 'static_assert', 'sizeof', 'decltype', 'alignof', 'noexcept',
})
# A function signature that declares at least one bool parameter: an identifier,
# then a parenthesised list (no nested parens, no ; { }) containing a bare bool
# token. Call sites never match, since a bool argument is a value, not the type
# token bool. Populated across the whole project before the per-file checks run.
_BOOL_PARAM_FUNCTION_RE = re.compile(r'\b(\w+)\s*\(([^;{}()]*\bbool\b[^;{}()]*)\)')
_PROJECT_BOOL_PARAM_FUNCTIONS: set = set()

def _collect_bool_param_functions(files: list[Path]) -> set:
    names = set()
    for path in files:
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        stripped_text = strip_comments_and_strings(text)
        for match in _BOOL_PARAM_FUNCTION_RE.finditer(stripped_text):
            name = match.group(1)
            if name not in _NON_CALL_CALLEES:
                names.add(name)
    return names

def _preceding_identifier(text: str, index: int) -> str:
    j = index - 1
    while j >= 0 and text[j] in ' \t\n':
        j -= 1
    if j >= 0 and text[j] == '>':  # step over a trailing template argument list: foo<...>(
        depth = 0
        while j >= 0:
            if text[j] == '>':
                depth += 1
            elif text[j] == '<':
                depth -= 1
                if depth == 0:
                    j -= 1
                    break
            j -= 1
        while j >= 0 and text[j] in ' \t\n':
            j -= 1
    end = j + 1
    while j >= 0 and (text[j].isalnum() or text[j] == '_'):
        j -= 1
    return text[j + 1:end]

def check_destructor_before_constructors(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    """Coding rule: "dtor before ctors".

    Where a class declares both a destructor and at least one constructor, the destructor
    comes first. The rule exists so a reader learns how an object is cleaned up before
    wading through however many ways it can be built, and so the ordering is one thing
    rather than a per-author preference.

    Only the declaration order within one class body is checked. Out-of-line definitions in
    a .cpp are not ordered by this rule -- there the destructor is often defaulted next to
    whatever forced it out of line.

    Deleted and defaulted constructors count: they are still constructor declarations, and
    the point is that the destructor heads the group.
    """
    stripped_text = ''.join(stripped)
    type_names = _constructible_type_names(stripped_text)
    if not type_names:
        return []

    line_starts = []
    offset = 0
    for line in stripped:
        line_starts.append(offset)
        offset += len(line)

    violations = []
    for type_name in sorted(type_names):
        escaped = re.escape(type_name)
        # A constructor declaration inside the class body: the type name, optional
        # specifiers, then an open paren. Excludes "~Name(" by requiring no leading tilde.
        ctor_re = re.compile(r'(?m)^[ \t]*(?:(?:explicit|constexpr|consteval|inline)\s+)*(?<![~\w])' + escaped + r'\s*\(')
        destructor_re = re.compile(r'(?m)^[ \t]*(?:(?:virtual|constexpr|inline)\s+)*~\s*' + escaped + r'\s*\(')

        destructor = destructor_re.search(stripped_text)
        if destructor is None:
            continue

        first_ctor = ctor_re.search(stripped_text)
        if first_ctor is None or first_ctor.start() > destructor.start():
            continue

        # An out-of-line definition writes "Name::Name(", which the constructor pattern
        # above would not match because of the "::". Nothing more to exclude here.
        line_number = bisect.bisect_right(line_starts, destructor.start())
        constructor_line = bisect.bisect_right(line_starts, first_ctor.start())
        violations.append(Violation(path, line_number,
                                    f"destructor of '{type_name}' is declared after its constructor on line "
                                    f"{constructor_line}; the coding rules require the destructor first"))
    return violations


def check_bool_literal_argument(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if _BOOL_LITERAL_EXEMPT_DIRS & set(path.parts):
        return []
    stripped_text = ''.join(stripped)
    length = len(stripped_text)

    line_starts = []
    offset = 0
    for line in stripped:
        line_starts.append(offset)
        offset += len(line)

    violations = []
    for match in _BOOL_LITERAL_RE.finditer(stripped_text):
        start = match.start()
        stop = match.end()

        left = start - 1
        while left >= 0 and stripped_text[left] in ' \t\n':
            left -= 1
        if left < 0 or stripped_text[left] not in '(,':
            continue  # not the first token of an argument

        right = stop
        while right < length and stripped_text[right] in ' \t\n':
            right += 1
        if right >= length or stripped_text[right] not in ',)':
            continue  # literal is part of a larger expression, not a bare argument

        depth = 0
        enclosing = ''
        enclosing_pos = -1
        scan = start - 1
        while scan >= 0:
            character = stripped_text[scan]
            if character in ')]}':
                depth += 1
            elif character in '([{':
                if depth == 0:
                    enclosing = character
                    enclosing_pos = scan
                    break
                depth -= 1
            scan -= 1
        if enclosing != '(':
            continue  # brace / bracket initialisation, not a call

        callee = _preceding_identifier(stripped_text, enclosing_pos)
        if callee not in _PROJECT_BOOL_PARAM_FUNCTIONS:
            continue  # not a project function that declares a bool parameter

        line_number = bisect.bisect_right(line_starts, start)
        if _BOOL_ARG_OK in lines[line_number - 1]:
            continue  # opted out: a genuinely unavoidable bool literal

        violations.append(Violation(path, line_number,
            f"bare '{match.group(0)}' passed to project function '{callee}(...)' which takes a bool "
            f"parameter; the call site cannot convey what it means -- give the parameter a typed flag "
            f"class (see UseHugePagesFlag), or mark a genuinely unavoidable case with a {_BOOL_ARG_OK} comment"))
    return violations


# ── Registry ─────────────────────────────────────────────────────────────────

# A fixed-size char/uint8_t buffer declaration, capturing the variable name. Matches
# both C arrays (char buf[N]) and std::array<char, N> buf (value, not reference/param).
_FIXED_BYTE_BUFFER_DECL = re.compile(
    r'\b(?:char|uint8_t|std::uint8_t)\s+(\w+)\s*\['
    r'|std::array\s*<\s*(?:char|uint8_t|std::uint8_t)\s*,[^>]*>\s+(\w+)\s*[;{=]')

# A message-encoding sink: a fixed buffer handed to any of these is the fragile pattern.
_ENCODE_SINK = re.compile(r'\b(?:encode|FixMessageWriter|serialise)\b')


def check_fixed_encode_buffer(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    """Flag a fixed-size byte/char buffer fed to a message encoder/writer.

    A variable-length message encoded into a fixed buffer silently drops or truncates on
    overflow. Use measure-then-fit or grow-and-retry with a reusable buffer instead.
    Scoped to production code (tests/benchmarks legitimately build fixed sample messages);
    annotate a deliberate exception with a 'fixed-buffer-ok' comment on the decl or use.
    """
    parts = set(path.parts)
    if parts & {'tests', 'tests_common', 'performance'} or path.name.endswith('Test.cpp'):
        return []
    violations: list[Violation] = []
    count = len(stripped)
    for i, line in enumerate(stripped):
        match = _FIXED_BYTE_BUFFER_DECL.search(line)
        if not match:
            continue
        name = match.group(1) or match.group(2)
        if not name:
            continue
        for j in range(i, min(i + 10, count)):
            probe = stripped[j]
            if name in re.split(r'\W+', probe) and _ENCODE_SINK.search(probe):
                if 'fixed-buffer-ok' in lines[i] or 'fixed-buffer-ok' in lines[j]:
                    break
                violations.append(Violation(path, i + 1,
                    f"fixed-size buffer '{name}' fed to a message encoder/writer; "
                    "use measure-then-fit or grow-and-retry with a reusable buffer "
                    "(a fixed cap silently drops an over-large message)"))
                break
    return violations


# ── Check 26: printf family ───────────────────────────────────────────────────

# Matched against the comment- and string-stripped text, so prose describing snprintf's
# contract (BumpAllocator's measuring mode documents itself that way) is not a violation.
# Only a call is: the name must be followed by an opening parenthesis.
_PRINTF_FAMILY_RE = re.compile(
    r'\b(?:std::)?(printf|fprintf|sprintf|snprintf|vprintf|vfprintf|vsprintf|vsnprintf)\s*\(')

def check_printf_family(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    """Flag any use of the printf family; fmt does the same job type-safely.

    printf format strings are unchecked against their arguments, so a mismatch is undefined
    behaviour rather than a diagnostic -- and every string_view argument needs a hand-written
    static_cast<int>(x.size()), x.data() pair that a refactor can silently break. gcc's
    -Wformat-truncation also rejects deliberate truncation, which cost a release.

    Use fmt::format_to / fmt::format_to_n into a caller-supplied or reusable buffer on any
    path where allocation matters, fmt::print to write straight to a stream, and fmt::format
    only where a returned std::string is wanted anyway.

    The one place neither is allowed is an async-signal-safe context -- a signal handler --
    where fmt and printf are equally unsafe and write(2) on a pre-formatted buffer is the
    only correct answer. Annotate such a use with 'signal-safe-ok' to accept it here.
    """
    violations: list[Violation] = []
    for i, line in enumerate(stripped):
        match = _PRINTF_FAMILY_RE.search(line)
        if not match:
            continue
        if 'signal-safe-ok' in lines[i]:
            continue
        name = match.group(1)
        violations.append(Violation(path, i + 1,
            f"'{name}' is banned; use fmt (format_to_n into a caller buffer, print to a "
            "stream, or format where a std::string is wanted). In a signal handler use "
            "write(2) -- neither printf nor fmt is async-signal-safe -- and mark it "
            "'signal-safe-ok'"))
    return violations


_CHECKS = [
    check_defines,
    check_screaming_snake_case,
    check_camel_case,
    check_null,
    check_std_size_t,
    check_using_namespace_std,
    check_noexcept,
    check_final,
    check_include_quotes,
    check_line_length,
    check_non_ascii,
    check_brace_comments,
    check_double_underscore,
    check_trailing_return_type,
    check_tabs,
    check_east_const,
    check_static_file_scope,
    check_s_prefix_static,
    check_doxygen_throws,
    check_pragma_once,
    check_template_on_own_line,
    check_banner_dividers,
    check_include_order,
    check_explicit_single_arg_ctor,
    check_destructor_before_constructors,
    check_bool_literal_argument,
    check_fixed_encode_buffer,
    check_printf_family,
]


# ── File discovery ────────────────────────────────────────────────────────────

def find_cpp_files(root: Path) -> list[Path]:
    files = []
    for top in ('libraries', 'applications'):
        scan_root = root / top
        if not scan_root.is_dir():
            continue
        for path in sorted(scan_root.rglob('*')):
            if path.suffix in ('.hpp', '.cpp'):
                files.append(path)
    return files


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description='Check C++ source files for coding-standard violations.',
    )
    parser.add_argument(
        'paths', nargs='*', type=Path,
        help='Files or directories to check. '
             'Default: scan from the project root (directory of this script).',
    )
    parser.add_argument(
        '--root', type=Path, default=Path(__file__).resolve().parent,
        help='Project root used when no explicit paths are given.',
    )
    args = parser.parse_args()

    if args.paths:
        files: list[Path] = []
        for path in args.paths:
            if path.is_dir():
                files.extend(find_cpp_files(path))
            elif path.suffix in ('.hpp', '.cpp'):
                files.append(path)
            else:
                print(f"warning: skipping {path} (not a .hpp or .cpp file)",
                      file=sys.stderr)
    else:
        files = find_cpp_files(args.root)

    if not files:
        print("No C++ files found.", file=sys.stderr)
        return 1

    # Collect project functions declaring a bool parameter from the whole tree,
    # not just the files being checked, so check 25 resolves callees correctly
    # even when a single file is passed on the command line.
    global _PROJECT_BOOL_PARAM_FUNCTIONS
    _PROJECT_BOOL_PARAM_FUNCTIONS = _collect_bool_param_functions(find_cpp_files(args.root))

    total = 0
    for path in files:
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError as exc:
            print(f"error: cannot read {path}: {exc}", file=sys.stderr)
            continue

        lines = text.splitlines(keepends=True)
        stripped_text = strip_comments_and_strings(text)
        stripped = stripped_text.splitlines(keepends=True)

        while len(stripped) < len(lines):
            stripped.append('\n')

        for check in _CHECKS:
            for violation in check(path, lines, stripped):
                print(violation)
                total += 1

    if total > 0:
        print(f"\n{total} violation(s) found.")
        return 1

    print("No violations found.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
