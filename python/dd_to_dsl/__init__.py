"""Generate a serialisation-DSL file from a FIX data dictionary plus a selection spec.

The FIX DD (parsed by :mod:`fix_dictionary`) is the source of truth for field types
and enumerated values; a hand-written TOML *selection spec* curates which messages and
which fields become PDUs. This keeps the internal PDUs derived from the DD -- so adding
a field (or, at a venue with custom tags, adding those tags to the DD) is a spec edit and
a regenerate, not hand-authoring of the ``.dsl``.
"""

from dd_to_dsl.spec import Spec, MessageSpec, ExtraField, load_spec
from dd_to_dsl.generator import generate_dsl

__all__ = ["Spec", "MessageSpec", "ExtraField", "load_spec", "generate_dsl"]
