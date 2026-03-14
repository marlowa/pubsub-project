import pytest
from dsl.lexer import Lexer
from dsl.errors import LexError

def test_lexer_skips_comments_and_whitespace():
    text = """
        # full-line comment
        message   Foo   # trailing comment
        end
    """
    lex = Lexer(text)
    kinds = []
    while True:
        tok = lex.next_token()
        kinds.append(tok.kind)
        if tok.kind == "EOF":
            break

    assert "KEYWORD" in kinds
    assert kinds.count("KEYWORD") == 2  # message, end


def test_lexer_colon_token():
    lex = Lexer("enum X : i32 { }")
    tokens = [lex.next_token().kind for _ in range(7)]
    assert "COLON" in tokens


def test_lexer_unexpected_character():
    with pytest.raises(LexError):
        Lexer("@bad").next_token()
