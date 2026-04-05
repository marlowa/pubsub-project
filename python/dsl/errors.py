"""DSL error types for the pubsub_itc_fw code generator."""


class DslError(Exception):
    """Base class for all DSL-related errors."""


class LexError(DslError):
    """Raised when the lexer encounters invalid characters or malformed tokens."""


class ParseError(DslError):
    """Raised when the parser encounters unexpected tokens or grammar violations."""


class ValidationError(DslError):
    """Raised when semantic validation fails (unknown types, cycles, duplicates, etc.)."""
