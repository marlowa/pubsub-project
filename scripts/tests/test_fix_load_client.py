"""Tests for what fix_load_client.py counts as an answer, and which orders it treats as resting."""

from pathlib import Path
import sys

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIR))

import fix_load_client  # noqa: E402

SOH = fix_load_client.SOH


class OneMessageSocket:
    """A socket that has the given bytes to read, once."""

    def __init__(self, data: bytes) -> None:
        self.data = data

    def settimeout(self, _timeout: float) -> None:
        pass

    def recv(self, _size: int) -> bytes:
        data, self.data = self.data, b""
        return data


def received(*fields_of_each_message) -> fix_load_client.FixSession:
    """A session that has read the given messages, each given as a list of (tag, value) pairs."""
    session = fix_load_client.FixSession(None, "CLIENT-1")
    wire = "".join(SOH.join(f"{tag}={value}" for tag, value in fields) + f"{SOH}10=000{SOH}" for fields in fields_of_each_message)
    session.socket = OneMessageSocket(wire.encode("ascii"))
    list(session.read_available(0))
    return session


def report(cl_ord_id: str, exec_type: str):
    return [(8, "FIXT.1.1"), (35, "8"), (11, cl_ord_id), (150, exec_type)]


def test_an_accepted_order_rests_and_a_rejected_one_does_not() -> None:
    session = received(report("ORDER-1", "0"), report("ORDER-2", "8"))
    assert session.resting == ["ORDER-1"]
    assert session.reports_received == 2


def test_an_order_cancel_reject_and_a_session_reject_each_answer_a_message() -> None:
    session = received([(8, "FIXT.1.1"), (35, "9"), (11, "ORDER-1-CANCEL")], [(8, "FIXT.1.1"), (35, "3"), (45, "7")])
    assert session.reports_received == 2
    assert session.resting == []
