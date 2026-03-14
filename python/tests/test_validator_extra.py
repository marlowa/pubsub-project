import pytest
from dsl.parser import Parser
from dsl.validator import Validator, ValidationError

def validate(text):
    ast = Parser(text).parse()
    Validator(ast).validate()
    return ast

def test_cycle_detection_simple():
    text = """
        message A (id=1)
            B b
        end
        message B (id=2)
            A a
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


def test_array_length_negative():
    text = """
        message Foo (id=1)
            i8[-1] bad
        end
    """
    with pytest.raises(ValidationError):
        validate(text)


def test_list_of_references_valid():
    text = """
        message Child (id=1)
            i32 x
        end
        message Parent (id=2)
            list<Child> kids
        end
    """
    ast = validate(text)
    parent = ast.declarations[1]
    assert parent.fields[0].type.element_type.name == "Child"
