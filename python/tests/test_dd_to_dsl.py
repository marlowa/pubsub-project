"""Tests for the full-message DD->DSL generator (dd_to_dsl)."""

import re

import pytest

from dd_to_dsl.__main__ import main
from dd_to_dsl.generator import GeneratorError, generate_dsl
from dd_to_dsl.spec import MessageSpec, Spec

# A small synthetic FIX dictionary exercising: a CHAR enum, an INT enum, a STRING field
# that carries values (must NOT become an enum), a MULTIPLECHARVALUE field (ditto), a
# plain CHAR field (-> char), a plain STRING field, an inlined component, and a repeating
# group with a nested group.
DD_XML = """<fix major="9" minor="9">
  <fields>
    <field number="11" name="ClOrdID" type="STRING"/>
    <field number="18" name="ExecInst" type="MULTIPLECHARVALUE"><value enum="1" description="NOT_HELD"/></field>
    <field number="21" name="HandlInst" type="CHAR"/>
    <field number="40" name="OrdType" type="CHAR"><value enum="1" description="MARKET"/><value enum="2" description="LIMIT"/></field>
    <field number="65" name="SymbolSfx" type="STRING"><value enum="CD" description="EUCP"/><value enum="WI" description="WHEN_ISSUED"/></field>
    <field number="103" name="OrdRejReason" type="INT"><value enum="0" description="BROKER"/><value enum="1" description="UNKNOWN_SYMBOL"/></field>
    <field number="448" name="PartyID" type="STRING"/>
    <field number="447" name="PartyIDSource" type="CHAR"><value enum="D" description="PROPRIETARY"/></field>
    <field number="452" name="PartyRole" type="INT"><value enum="1" description="EXECUTING_FIRM"/></field>
    <field number="453" name="NoPartyIDs" type="NUMINGROUP"/>
    <field number="523" name="PartySubID" type="STRING"/>
    <field number="802" name="NoPartySubIDs" type="NUMINGROUP"/>
  </fields>
  <components>
    <component name="Parties">
      <group name="NoPartyIDs" required="N">
        <field name="PartyID" required="Y"/>
        <field name="PartyIDSource" required="N"/>
        <field name="PartyRole" required="N"/>
        <group name="NoPartySubIDs" required="N">
          <field name="PartySubID" required="N"/>
        </group>
      </group>
    </component>
  </components>
  <messages>
    <message name="DemoOrder" msgtype="D" msgcat="app">
      <field name="ClOrdID" required="Y"/>
      <field name="OrdType" required="Y"/>
      <field name="HandlInst" required="N"/>
      <field name="SymbolSfx" required="N"/>
      <field name="ExecInst" required="N"/>
      <field name="OrdRejReason" required="N"/>
      <component name="Parties" required="N"/>
    </message>
  </messages>
</fix>
"""


def _generate(tmp_path, messages=None):
    dd = tmp_path / "demo.xml"
    dd.write_text(DD_XML, encoding="utf-8")
    spec = Spec(
        output="out.dsl",
        data_dictionary=["demo.xml"],
        enums=[],
        messages=messages if messages is not None else [MessageSpec(name="DemoOrder", pdu_id=1000)],
    )
    return generate_dsl(spec, tmp_path)


def _message_body(dsl, name):
    match = re.search(rf"^message {name} .*?\n(.*?)\nend", dsl, re.DOTALL | re.MULTILINE)
    assert match, f"message {name} not found"
    return match.group(1)


def test_char_field_with_values_is_a_char_enum(tmp_path):
    dsl = _generate(tmp_path)
    assert "enum OrdType : char {" in dsl
    assert "Market = '1'" in dsl and "Limit = '2'" in dsl
    assert "OrdType ord_type" in _message_body(dsl, "DemoOrder")


def test_int_field_with_values_is_an_i32_enum(tmp_path):
    dsl = _generate(tmp_path)
    assert "enum OrdRejReason : i32 {" in dsl
    assert "Broker = 0" in dsl and "UnknownSymbol = 1" in dsl
    assert "OrdRejReason ord_rej_reason" in _message_body(dsl, "DemoOrder")


def test_string_field_with_values_is_not_an_enum(tmp_path):
    dsl = _generate(tmp_path)
    assert "enum SymbolSfx" not in dsl
    assert "string symbol_sfx" in _message_body(dsl, "DemoOrder")


def test_multiplecharvalue_field_is_not_an_enum(tmp_path):
    dsl = _generate(tmp_path)
    assert "enum ExecInst" not in dsl
    assert "string exec_inst" in _message_body(dsl, "DemoOrder")


def test_plain_char_field_maps_to_char_not_string(tmp_path):
    # Preserves the int-vs-char display distinction: a non-enum CHAR is a `char`.
    dsl = _generate(tmp_path)
    assert "char handl_inst" in _message_body(dsl, "DemoOrder")


def test_component_is_inlined_not_emitted_as_a_message(tmp_path):
    dsl = _generate(tmp_path)
    assert "message Parties" not in dsl  # the component is spliced in, not its own message


def test_repeating_group_becomes_list_and_nested_group_precedes_parent(tmp_path):
    dsl = _generate(tmp_path)
    # The group appears on the parent as a list of its body message (No-prefix dropped).
    assert "list<PartyIDs> no_party_i_ds" in _message_body(dsl, "DemoOrder")
    # Nested group body message must be defined before the group that references it.
    assert dsl.index("message PartySubIDs ") < dsl.index("message PartyIDs "), "nested group must be declared before its parent"
    assert "list<PartySubIDs>" in _message_body(dsl, "PartyIDs")


def test_required_vs_optional_follows_the_dd(tmp_path):
    body = _message_body(_generate(tmp_path), "DemoOrder")
    assert re.search(r"^    string cl_ord_id", body, re.MULTILINE)          # required -> no 'optional'
    assert re.search(r"^    optional char handl_inst", body, re.MULTILINE)  # required='N' -> optional


def test_unknown_message_raises(tmp_path):
    with pytest.raises(GeneratorError):
        _generate(tmp_path, messages=[MessageSpec(name="NoSuchMessage", pdu_id=1)])


def test_cli_all_assigns_ids_from_base(tmp_path, capsys):
    dd = tmp_path / "demo.xml"
    dd.write_text(DD_XML, encoding="utf-8")
    rc = main(["--dd", str(dd), "--all", "--id-base", "1000", "--stdout"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "DemoOrder = 1000" in out


def test_cli_explicit_message_and_bad_arg(tmp_path, capsys):
    dd = tmp_path / "demo.xml"
    dd.write_text(DD_XML, encoding="utf-8")
    assert main(["--dd", str(dd), "--message", "DemoOrder:2000", "--stdout"]) == 0
    assert "DemoOrder = 2000" in capsys.readouterr().out
    assert main(["--dd", str(dd), "--message", "DemoOrder", "--stdout"]) == 1  # missing :id
