"""Tests of the plain-Python generator (dsl/generator_python.py) and the PDU framing (dsl/pdu_framing.py).

The first group checks the Python and Java generators against the C++ one byte for byte, through the programs in
tests/cross_language.py; it needs cmake and cpptrace, as the pybind11 tests do, and Java 17 for the Java part. The
rest check the Python code on its own.
"""

import ast as python_ast
import glob
import importlib.util
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

from dsl import pdu_framing
from dsl.generator_python import PythonGenerator
from dsl.loader import load
from dsl.parser import Parser
from dsl.validator import Validator
from tests import cross_language

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

needs_cpp = pytest.mark.skipif(not cross_language.environment_ready(),
                               reason="the C++ reference program needs cmake and cpptrace (set THIRDPARTY_DIR and CPPTRACE_VERSION)")
needs_java = pytest.mark.skipif(not cross_language.java_available(), reason="the Java check needs javac and java on the path")


@pytest.fixture(scope="module")
def every_type():
    """The test DSL file's AST, its generated Python module, and its message declarations."""
    tree = cross_language.load_every_type()
    module = cross_language.load_python_module(tree)
    messages, _ = cross_language._declarations(tree)  # pylint: disable=protected-access
    return tree, module, messages


@pytest.fixture(scope="module")
def cpp_bytes(every_type):  # pylint: disable=redefined-outer-name
    """Each case's bytes, and each header case's whole PDU, as the C++ code writes them."""
    tree, _, _ = every_type
    return cross_language.run_cpp(tree, cross_language.CASES, cross_language.HEADER_CASES)


def _message(every_type, message_name, values):  # pylint: disable=redefined-outer-name
    _, module, messages = every_type
    return cross_language.build_python_message(module, messages, message_name, values)


# ----------------------------------------------------------------------------------------------------------------
# Against the C++ code
# ----------------------------------------------------------------------------------------------------------------

@needs_cpp
@pytest.mark.parametrize("case_name,message_name,values", cross_language.CASES, ids=[c[0] for c in cross_language.CASES])
def test_python_writes_the_same_bytes_as_cpp(every_type, cpp_bytes, case_name, message_name, values):  # pylint: disable=redefined-outer-name
    assert _message(every_type, message_name, values).encode() == cpp_bytes[case_name]


@needs_cpp
@pytest.mark.parametrize("case_name,message_name,values", cross_language.CASES, ids=[c[0] for c in cross_language.CASES])
def test_python_decodes_the_cpp_bytes_to_the_same_values(every_type, cpp_bytes, case_name, message_name, values):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    decoded, end = getattr(module, message_name).decode(cpp_bytes[case_name])
    assert end == len(cpp_bytes[case_name])
    assert decoded == _message(every_type, message_name, values)


@needs_cpp
@pytest.mark.parametrize("case", cross_language.HEADER_CASES, ids=[c[0] for c in cross_language.HEADER_CASES])
def test_python_frames_a_pdu_as_the_framework_does(cpp_bytes, case):  # pylint: disable=redefined-outer-name
    case_name, pdu_id, version, seq_no, payload = case
    assert pdu_framing.encode_pdu(pdu_id, version, payload, seq_no) == cpp_bytes[case_name]
    pdus = pdu_framing.PduReader().feed(cpp_bytes[case_name])
    assert len(pdus) == 1
    header, body = pdus[0]
    assert (header.pdu_id, header.version, header.seq_no, header.byte_count, body) == (pdu_id, version, seq_no, len(payload), payload)


# Java decodes an enum value its enum does not list as null, and cannot encode the message again (BUG-0179), so the
# case holding such values is left out of the Java checks.
_JAVA_CASES = [c for c in cross_language.CASES if "unlisted" not in c[0]]


@pytest.fixture(scope="module")
def java_results(every_type, cpp_bytes):  # pylint: disable=redefined-outer-name
    tree, _, _ = every_type
    reencode_input = {case_name: (message_name, cpp_bytes[case_name]) for case_name, message_name, _ in _JAVA_CASES}
    return cross_language.run_java(tree, _JAVA_CASES, reencode_input)


@needs_cpp
@needs_java
@pytest.mark.parametrize("case_name,message_name,values", _JAVA_CASES, ids=[c[0] for c in _JAVA_CASES])
def test_java_writes_the_same_bytes_as_cpp(java_results, cpp_bytes, case_name, message_name, values):  # pylint: disable=redefined-outer-name,unused-argument
    encoded, _ = java_results
    assert encoded[case_name] == cpp_bytes[case_name]


@needs_cpp
@needs_java
@pytest.mark.parametrize("case_name,message_name,values", _JAVA_CASES, ids=[c[0] for c in _JAVA_CASES])
def test_java_decodes_and_encodes_the_cpp_bytes_unchanged(java_results, cpp_bytes, case_name, message_name, values):  # pylint: disable=redefined-outer-name,unused-argument
    _, again = java_results
    assert again[case_name] == cpp_bytes[case_name].hex()


# ----------------------------------------------------------------------------------------------------------------
# The Python code on its own
# ----------------------------------------------------------------------------------------------------------------

def _generate(dsl_text: str):
    tree = Parser(dsl_text).parse()
    Validator(tree).validate()
    return _import(PythonGenerator(source_name="test.dsl").emit(tree))


def _import(source: str):
    directory = _PROJECT_ROOT / "build" / "python_generator_tests"
    directory.mkdir(parents=True, exist_ok=True)
    name = f"generated_{uuid.uuid4().hex}"
    path = directory / f"{name}.py"
    path.write_text(source)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    path.unlink()
    return module


def _project_dsl_files():
    patterns = ["libraries/**/*.dsl", "applications/**/*.dsl", "build/generated_dsl/**/*.dsl"]
    found = set()
    for pattern in patterns:
        found.update(glob.glob(str(_PROJECT_ROOT / pattern), recursive=True))
    return sorted(f for f in found if "/tests/" not in f)


@pytest.mark.parametrize("path", _project_dsl_files(), ids=lambda p: Path(p).name)
def test_every_project_dsl_file_generates_imports_and_round_trips(path):
    tree = None
    for pdu_id_enum in (False, True):
        try:
            tree = load(Path(path), include_dirs=[str(_PROJECT_ROOT / "build" / "generated_dsl")])
            Validator(tree).validate(pdu_id_enum=pdu_id_enum)
            break
        except Exception:  # pylint: disable=broad-except
            tree = None
    if tree is None:
        pytest.skip(f"{Path(path).name} does not load or validate on its own here")
    source = PythonGenerator(source_name=Path(path).name).emit(tree)
    python_ast.parse(source, feature_version=(3, 8))
    module = _import(source)
    for message_class in module.MESSAGES_BY_ID.values():
        default = message_class()
        decoded, end = message_class.decode(default.encode())
        assert decoded == default and end == len(default.encode())


def test_generated_code_and_framing_parse_as_python_3_8(every_type):  # pylint: disable=redefined-outer-name
    tree, _, _ = every_type
    python_ast.parse(PythonGenerator().emit(tree), feature_version=(3, 8))
    python_ast.parse(Path(pdu_framing.__file__).read_text(encoding="utf-8"), feature_version=(3, 8))


def test_messages_by_id_maps_each_message_id_to_its_class(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    assert module.MESSAGES_BY_ID[1] is module.Integers
    assert module.MESSAGES_BY_ID[8] is module.Optionals
    assert module.Integers.MESSAGE_ID == 1


def test_a_buffer_cut_short_anywhere_raises_decode_error_naming_the_field(every_type):  # pylint: disable=redefined-outer-name
    case = next(c for c in cross_language.CASES if c[0] == "lists_full")
    message = _message(every_type, case[1], case[2])
    encoded = message.encode()
    _, module, _ = every_type
    for cut in range(len(encoded)):
        with pytest.raises(module.DecodeError, match=r"^(Lists|Inner)\."):
            module.Lists.decode(encoded[:cut])


def test_an_optional_field_missing_at_the_end_of_the_buffer_decodes_as_absent(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    only_required = (7).to_bytes(4, "little", signed=True)
    decoded, end = module.Optionals.decode(only_required)
    assert end == 4
    assert decoded == module.Optionals(required=7)
    assert decoded.tiny is None and decoded.numbers is None


def test_a_value_out_of_its_range_raises_encode_error_naming_the_field(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    with pytest.raises(module.EncodeError, match="Integers.tiny"):
        module.Integers(tiny=128).encode()
    with pytest.raises(module.EncodeError, match="Integers.wide"):
        module.Integers(wide=2 ** 63).encode()


def test_a_fixed_array_of_the_wrong_length_raises_encode_error(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    with pytest.raises(module.EncodeError, match="Arrays.numbers"):
        module.Arrays(numbers=[1, 2]).encode()


def test_a_char_must_be_one_character_below_256(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    for bad in ("", "ab", "✓"):
        with pytest.raises(module.EncodeError, match="Integers.letter"):
            module.Integers(letter=bad).encode()


def test_an_unlisted_enum_value_is_kept_and_first_invalid_field_names_it(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    encoded = module.Enums(small=5).encode()
    decoded, _ = module.Enums.decode(encoded)
    assert decoded.small == 5 and not isinstance(decoded.small, module.Small)
    assert decoded.first_invalid_field() == "small"
    assert decoded.encode() == encoded
    assert module.Enums().first_invalid_field() is None


def test_first_invalid_field_names_an_empty_required_string_but_not_an_empty_optional_one(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    assert module.Text(name="").first_invalid_field() == "name"
    assert module.Text(name="x").first_invalid_field() is None
    assert module.Optionals(name="").first_invalid_field() is None


def test_first_invalid_field_looks_inside_nested_messages_and_lists_of_them(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    assert module.Nested(inner=module.Inner(label="")).first_invalid_field() == "label"
    assert module.Nested(inner=module.Inner(label="x"), maybe_inner=module.Inner(label="")).first_invalid_field() == "label"
    assert module.Lists(inners=[module.Inner(label="x"), module.Inner(label="")]).first_invalid_field() == "label"


def test_a_string_that_is_not_utf8_survives_decoding_and_encoding(every_type):  # pylint: disable=redefined-outer-name
    _, module, _ = every_type
    not_utf8 = b"\xff\xfe\x80"
    encoded = len(not_utf8).to_bytes(4, "little") + not_utf8 + (0).to_bytes(4, "little")
    decoded, _ = module.Text.decode(encoded)
    assert decoded.encode() == encoded


def test_a_list_of_enums_round_trips():
    # Not checked against the C++ code, which cannot yet write a list of enums (BUG-0177).
    module = _generate("enum Colour : i16 {\n Red = 1\n Blue = -2\n}\nmessage M (id=1)\n list<Colour> colours\nend\n")
    message = module.M(colours=[module.Colour.Red, module.Colour.Blue, 9])
    encoded = message.encode()
    assert encoded == (3).to_bytes(4, "little") + b"\x01\x00\xfe\xff\x09\x00"
    decoded, _ = module.M.decode(encoded)
    assert decoded.colours == [1, -2, 9] and decoded.first_invalid_field() == "colours"


def test_names_that_are_python_keywords_gain_an_underscore():
    module = _generate("enum E : i8 {\n None = 0\n}\nmessage M (id=1)\n i32 from\n E class\nend\n")
    message = module.M(from_=3, class_=module.E.None_)
    decoded, _ = module.M.decode(message.encode())
    assert decoded.from_ == 3 and decoded.class_ == 0


# ----------------------------------------------------------------------------------------------------------------
# Framing on its own
# ----------------------------------------------------------------------------------------------------------------

def _stream():
    return (pdu_framing.encode_pdu(1, 1, b"first", 10) + pdu_framing.encode_pdu(2, 1, b"", 0)
            + pdu_framing.encode_pdu(3, 2, bytes(300), -1))


def test_pdus_split_across_reads_or_joined_in_one_come_out_whole_and_in_order():
    whole = pdu_framing.PduReader().feed(_stream())
    assert [(h.pdu_id, body) for h, body in whole] == [(1, b"first"), (2, b""), (3, bytes(300))]
    reader = pdu_framing.PduReader()
    one_byte_at_a_time = []
    for index in range(len(_stream())):
        one_byte_at_a_time.extend(reader.feed(_stream()[index:index + 1]))
    assert one_byte_at_a_time == whole
    assert reader.pending_bytes == 0


def test_an_incomplete_pdu_is_held_until_the_rest_arrives():
    reader = pdu_framing.PduReader()
    pdu = pdu_framing.encode_pdu(5, 1, b"abcdef")
    assert not reader.feed(pdu[:-1])
    assert reader.pending_bytes == len(pdu) - 1
    assert len(reader.feed(pdu[-1:])) == 1


def test_a_wrong_canary_raises_framing_error():
    pdu = bytearray(pdu_framing.encode_pdu(5, 1, b"abc"))
    pdu[16] ^= 0xFF
    with pytest.raises(pdu_framing.FramingError, match="canary"):
        pdu_framing.PduReader().feed(bytes(pdu))


def test_the_command_line_writes_a_python_module():
    output = _PROJECT_ROOT / "build" / "python_generator_tests" / f"cli_{uuid.uuid4().hex}.py"
    tool = _PROJECT_ROOT / "python" / "tools" / "generate_cpp_from_dsl.py"
    subprocess.run([sys.executable, str(tool), str(cross_language.EVERY_TYPE_DSL), "--python", str(output)], check=True)
    module = _import(output.read_text())
    assert module.MESSAGES_BY_ID[2] is module.Text
    output.unlink()
