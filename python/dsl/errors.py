class DslError(Exception):
    """Base class for all DSL-related errors."""
    pass


class LexError(DslError):
    """Raised when the lexer encounters invalid characters or malformed tokens."""
    pass


class ParseError(DslError):
    """Raised when the parser encounters unexpected tokens or grammar violations."""
    pass


class ValidationError(DslError):
    """Raised when semantic validation fails (unknown types, cycles, duplicates, etc.)."""
    pass
