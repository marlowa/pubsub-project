"""FIX data-dictionary reader and C++ header emitter.

This package parses one or more QuickFIX-style FIX XML data dictionaries
(for example ``FIXT11.xml`` and ``FIX50SP2.xml``) into a small in-memory
model and emits a zero-copy-friendly C++17 header of tag numbers, message
types, enumerated field values, a tag->name table, and the DATA/LENGTH
field pairing used by a FIX parser to read binary data fields by length.

The design follows the ideas of the hffix library (generated field metadata,
no runtime allocation) but the generator is pure-Python and reads the single
QuickFIX-style dictionary file rather than the FIX Repository.
"""

from .model import Dictionary, FieldDef, FieldValue, MessageDef
from .parser import DictionaryError, parse_dictionaries
from .emitter import emit_header

__all__ = [
    "Dictionary",
    "FieldDef",
    "FieldValue",
    "MessageDef",
    "DictionaryError",
    "parse_dictionaries",
    "emit_header",
]
