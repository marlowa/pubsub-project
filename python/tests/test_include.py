"""Tests for the DSL `include` mechanism (lexer STRING token + loader)."""

import pytest

from dsl.lexer import Lexer
from dsl.parser import Parser
from dsl.loader import load, Loader
from dsl.validator import Validator
from dsl.errors import ParseError, LexError, ValidationError
from dsl.ast import IncludeDecl, MessageDecl, EnumDecl


# -----------------------------------------------------------------------------
# Lexer / parser: the STRING token and the include declaration
# -----------------------------------------------------------------------------

def _tokens(text):
    lexer = Lexer(text)
    out = []
    while True:
        tok = lexer.next_token()
        out.append(tok)
        if tok.kind == "EOF":
            return out


def test_lexer_produces_string_token():
    toks = _tokens('include "foo/bar.dsl"')
    kinds = [t.kind for t in toks]
    assert kinds == ["KEYWORD", "STRING", "EOF"]
    assert toks[0].value == "include"
    assert toks[1].value == "foo/bar.dsl"


def test_lexer_unterminated_string_is_an_error():
    with pytest.raises(LexError, match="Unterminated string"):
        _tokens('include "no closing quote')


def test_parser_yields_include_decl():
    ast = Parser('include "other.dsl"').parse()
    assert len(ast.declarations) == 1
    decl = ast.declarations[0]
    assert isinstance(decl, IncludeDecl)
    assert decl.path == "other.dsl"


def test_parser_rejects_empty_include_path():
    with pytest.raises(ParseError, match="Empty include path"):
        Parser('include ""').parse()


# -----------------------------------------------------------------------------
# Loader helpers
# -----------------------------------------------------------------------------

def _write(directory, name, text):
    path = directory / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def _names(ast):
    return [d.name for d in ast.declarations if isinstance(d, (MessageDecl, EnumDecl))]


# -----------------------------------------------------------------------------
# Loader: transitive expansion
# -----------------------------------------------------------------------------

def test_no_includes_returns_declarations_unchanged(tmp_path):
    root = _write(tmp_path, "root.dsl", """
        message Foo (id=1)
            i32 x
        end
    """)
    ast = load(root)
    assert _names(ast) == ["Foo"]
    assert not any(isinstance(d, IncludeDecl) for d in ast.declarations)


def test_single_include_merges_declarations(tmp_path):
    _write(tmp_path, "leaf.dsl", """
        message Leaf (id=2)
            i32 y
        end
    """)
    root = _write(tmp_path, "root.dsl", """
        include "leaf.dsl"
        message Root (id=1)
            i32 x
        end
    """)
    ast = load(root)
    assert _names(ast) == ["Leaf", "Root"]
    # IncludeDecl nodes never survive into the merged AST.
    assert not any(isinstance(d, IncludeDecl) for d in ast.declarations)


def test_nested_include_is_transitive(tmp_path):
    _write(tmp_path, "c.dsl", "message C (id=3)\n i32 z\nend\n")
    _write(tmp_path, "b.dsl", 'include "c.dsl"\nmessage B (id=2)\n i32 y\nend\n')
    root = _write(tmp_path, "a.dsl", 'include "b.dsl"\nmessage A (id=1)\n i32 x\nend\n')
    ast = load(root)
    assert _names(ast) == ["C", "B", "A"]


def test_diamond_include_dedups(tmp_path):
    # a includes b and c; both b and c include d. d must appear exactly once.
    _write(tmp_path, "d.dsl", "message D (id=4)\n i32 w\nend\n")
    _write(tmp_path, "b.dsl", 'include "d.dsl"\nmessage B (id=2)\n i32 y\nend\n')
    _write(tmp_path, "c.dsl", 'include "d.dsl"\nmessage C (id=3)\n i32 z\nend\n')
    root = _write(tmp_path, "a.dsl", 'include "b.dsl"\ninclude "c.dsl"\nmessage A (id=1)\n i32 x\nend\n')
    ast = load(root)
    assert _names(ast).count("D") == 1
    assert set(_names(ast)) == {"A", "B", "C", "D"}


def test_include_path_is_relative_to_including_file(tmp_path):
    # The leaf lives in a subdir and is referenced relative to its includer,
    # which itself lives in that subdir -- not relative to the root's directory.
    _write(tmp_path, "sub/leaf.dsl", "message Leaf (id=2)\n i32 y\nend\n")
    _write(tmp_path, "sub/mid.dsl", 'include "leaf.dsl"\nmessage Mid (id=3)\n i32 m\nend\n')
    root = _write(tmp_path, "root.dsl", 'include "sub/mid.dsl"\nmessage Root (id=1)\n i32 x\nend\n')
    ast = load(root)
    assert set(_names(ast)) == {"Leaf", "Mid", "Root"}


# -----------------------------------------------------------------------------
# Loader: error cases
# -----------------------------------------------------------------------------

def test_missing_include_is_an_error(tmp_path):
    root = _write(tmp_path, "root.dsl", 'include "nope.dsl"\n')
    with pytest.raises(ParseError, match="Included file not found"):
        load(root)


def test_missing_root_is_an_error(tmp_path):
    with pytest.raises(ParseError, match="DSL file not found"):
        load(tmp_path / "does_not_exist.dsl")


def test_self_cycle_is_detected(tmp_path):
    root = _write(tmp_path, "root.dsl", 'include "root.dsl"\n')
    with pytest.raises(ParseError, match="Cyclic include detected"):
        load(root)


def test_mutual_cycle_is_detected(tmp_path):
    _write(tmp_path, "b.dsl", 'include "a.dsl"\nmessage B (id=2)\n i32 y\nend\n')
    root = _write(tmp_path, "a.dsl", 'include "b.dsl"\nmessage A (id=1)\n i32 x\nend\n')
    with pytest.raises(ParseError, match="Cyclic include detected"):
        load(root)


def test_cycle_error_reports_the_chain(tmp_path):
    _write(tmp_path, "b.dsl", 'include "a.dsl"\n')
    root = _write(tmp_path, "a.dsl", 'include "b.dsl"\n')
    with pytest.raises(ParseError) as exc:
        load(root)
    message = str(exc.value)
    assert "a.dsl" in message and "b.dsl" in message and "->" in message


# -----------------------------------------------------------------------------
# Loader + validator: whole-program checks span included files
# -----------------------------------------------------------------------------

def test_duplicate_id_across_includes_is_rejected(tmp_path):
    _write(tmp_path, "leaf.dsl", "message Leaf (id=1)\n i32 y\nend\n")
    root = _write(tmp_path, "root.dsl", 'include "leaf.dsl"\nmessage Root (id=1)\n i32 x\nend\n')
    ast = load(root)
    with pytest.raises(ValidationError):
        Validator(ast).validate()


def test_valid_across_includes_passes_validation(tmp_path):
    _write(tmp_path, "leaf.dsl", "message Leaf (id=2)\n i32 y\nend\n")
    root = _write(tmp_path, "root.dsl", 'include "leaf.dsl"\nmessage Root (id=1)\n i32 x\nend\n')
    ast = load(root)
    Validator(ast).validate()  # must not raise


def test_loader_instance_is_single_use_but_load_helper_is_repeatable(tmp_path):
    _write(tmp_path, "leaf.dsl", "message Leaf (id=2)\n i32 y\nend\n")
    root = _write(tmp_path, "root.dsl", 'include "leaf.dsl"\nmessage Root (id=1)\n i32 x\nend\n')
    # The module-level helper builds a fresh Loader each call, so repeated loads
    # of the same file are independent and identical.
    first = _names(load(root))
    second = _names(load(root))
    assert first == second == ["Leaf", "Root"]
    # A hand-built Loader also works for a one-shot load.
    assert _names(Loader().load(root)) == ["Leaf", "Root"]
