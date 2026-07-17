"""Tests for the FIX data-dictionary parser and C++ header emitter."""

from pathlib import Path

import pytest

from fix_dictionary import DictionaryError, emit_header, parse_dictionaries

REPO_ROOT = Path(__file__).resolve().parents[2]
DATA_DICTIONARY_DIR = REPO_ROOT / "libraries" / "fix_codec" / "data_dictionary"

SESSION_XML = """<fix type='FIXT' major='1' minor='1' servicepack='0'>
 <header />
 <messages>
  <message name='Heartbeat' msgtype='0' msgcat='admin' />
  <message name='Logon' msgtype='A' msgcat='admin' />
 </messages>
 <fields>
  <field number='8' name='BeginString' type='STRING' />
  <field number='9' name='BodyLength' type='LENGTH' />
  <field number='35' name='MsgType' type='STRING' />
 </fields>
</fix>
"""

APP_XML = """<fix type='FIX' major='5' minor='0' servicepack='2'>
 <header />
 <messages>
  <message name='NewOrderSingle' msgtype='D' msgcat='app' />
 </messages>
 <fields>
  <field number='8' name='BeginString' type='STRING' />
  <field number='11' name='ClOrdID' type='STRING' />
  <field number='54' name='Side' type='CHAR'>
   <value enum='1' description='BUY' />
   <value enum='2' description='SELL' />
  </field>
  <field number='93' name='SignatureLength' type='LENGTH' />
  <field number='89' name='Signature' type='DATA' />
 </fields>
</fix>
"""


def _write(tmp_path, name, text):
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return path


def test_parse_single_dictionary(tmp_path):
    dictionary = parse_dictionaries([_write(tmp_path, "app.xml", APP_XML)])
    assert dictionary.fields[11].name == "ClOrdID"
    assert dictionary.fields[54].type == "CHAR"
    assert [value.description for value in dictionary.fields[54].values] == ["BUY", "SELL"]
    assert dictionary.messages["D"].name == "NewOrderSingle"


def test_merge_unions_fields_and_messages(tmp_path):
    dictionary = parse_dictionaries([
        _write(tmp_path, "session.xml", SESSION_XML),
        _write(tmp_path, "app.xml", APP_XML),
    ])
    # Session tags and application tags coexist; tag 8 is defined in both.
    assert dictionary.fields[35].name == "MsgType"
    assert dictionary.fields[11].name == "ClOrdID"
    assert dictionary.messages["A"].name == "Logon"
    assert dictionary.messages["D"].name == "NewOrderSingle"


def test_data_length_pairing_is_by_name(tmp_path):
    # Signature (89, DATA) pairs with SignatureLength (93, LENGTH) despite the
    # non-adjacent tag numbers -- proving the pairing is by name, not by number.
    dictionary = parse_dictionaries([_write(tmp_path, "app.xml", APP_XML)])
    assert dictionary.data_length_pairs() == [(93, 89)]


def test_conflicting_definition_is_rejected(tmp_path):
    conflict = APP_XML.replace("name='ClOrdID'", "name='Renamed'")
    with pytest.raises(DictionaryError):
        parse_dictionaries([
            _write(tmp_path, "app.xml", APP_XML),
            _write(tmp_path, "conflict.xml", conflict),
        ])


def test_emitted_header_has_expected_symbols(tmp_path):
    dictionary = parse_dictionaries([
        _write(tmp_path, "session.xml", SESSION_XML),
        _write(tmp_path, "app.xml", APP_XML),
    ])
    header = emit_header(dictionary, namespace="fix_codec")
    assert "#pragma once" in header
    assert "namespace fix_codec {" in header
    assert "inline constexpr int ClOrdID = 11;" in header
    assert 'inline constexpr std::string_view NewOrderSingle = "D";' in header
    assert "inline constexpr char BUY = '1';" in header
    assert "{93, 89}," in header
    assert "inline constexpr bool is_data_length_tag(int length_tag)" in header


@pytest.mark.skipif(not DATA_DICTIONARY_DIR.is_dir(), reason="bundled FIX dictionaries not present")
def test_real_dictionaries_generate_cleanly():
    inputs = [DATA_DICTIONARY_DIR / "FIXT11.xml", DATA_DICTIONARY_DIR / "FIX50SP2.xml"]
    dictionary = parse_dictionaries(inputs)
    # Sanity: the well-known session and application tags are present.
    assert dictionary.fields[35].name == "MsgType"
    assert dictionary.fields[11].name == "ClOrdID"
    header = emit_header(dictionary, namespace="fix_codec")
    assert "inline constexpr int CheckSum = 10;" in header
    # RawDataLength (95) -> RawData (96) is one of the derived data-length pairs.
    assert "{95, 96}," in header
