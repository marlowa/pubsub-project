# pylint: disable=redefined-outer-name
"""Tests of the high availability tests' end of the test event channel (scripts/test_event_listener.py).

A component is played by a socket in the test, sending what the library's TestEventChannel sends. The generated
test_events module is made from the library's DSL file into the build tree, as the build makes it.
"""

import socket
import sys
import threading
import time
from pathlib import Path

import pytest

from dsl.generator_python import PythonGenerator
from dsl.loader import load
from dsl.validator import Validator
from dsl import pdu_framing

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
_PYTHON_DIR = _PROJECT_ROOT / "build" / "test_event_listener_tests"
sys.path.insert(0, str(_PROJECT_ROOT / "scripts"))

import test_event_listener  # noqa: E402  pylint: disable=wrong-import-position,wrong-import-order


@pytest.fixture(scope="module", autouse=True)
def generated_modules():
    """test_events.py, generated from the library's DSL file, beside a copy of pdu_framing.py."""
    _PYTHON_DIR.mkdir(parents=True, exist_ok=True)
    tree = load(_PROJECT_ROOT / "libraries" / "pubsub_itc_fw" / "include" / "pubsub_itc_fw" / "test_events.dsl")
    Validator(tree).validate()
    (_PYTHON_DIR / "test_events.py").write_text(PythonGenerator(source_name="test_events.dsl").emit(tree))
    (_PYTHON_DIR / "pdu_framing.py").write_text(Path(pdu_framing.__file__).read_text(encoding="utf-8"))


@pytest.fixture
def listener():
    made = test_event_listener.TestEventListener(_PYTHON_DIR)
    yield made
    made.close()


def _component(listener, name="sequencer_primary"):
    """A connected socket that has introduced itself as name's SequencerThread."""
    sock = socket.create_connection((listener.host, listener.port))
    events = listener.messages
    hello = events.TestEventsHello(component=name, instance=1, thread="SequencerThread")
    sock.sendall(pdu_framing.encode_pdu(events.TestEventsHello.MESSAGE_ID, 1, hello.encode()))
    return sock


def _role_changed(listener, to_role, epoch=1):
    events = listener.messages
    message = events.RoleChanged(owner="sequencer", instance=1, from_role=events.LeadershipRole.Unknown, to_role=to_role, epoch=epoch)
    return pdu_framing.encode_pdu(events.RoleChanged.MESSAGE_ID, 1, message.encode())


def test_an_event_sent_a_byte_at_a_time_arrives_whole_with_its_sender(listener):
    sock = _component(listener)
    for byte in _role_changed(listener, listener.messages.LeadershipRole.Leader, epoch=7):
        sock.sendall(bytes([byte]))
    found = listener.wait_for(test_event_listener.leads(listener.messages, "sequencer_primary"), timeout=5.0)
    assert found is not None
    assert found.component == "sequencer_primary" and found.thread == "SequencerThread" and found.message.epoch == 7
    sock.close()


def test_events_joined_in_one_send_all_arrive_in_order(listener):
    sock = _component(listener)
    roles = listener.messages.LeadershipRole
    sock.sendall(_role_changed(listener, roles.Follower, 1) + _role_changed(listener, roles.Leader, 2) + _role_changed(listener, roles.Follower, 3))
    assert listener.wait_for(lambda e: e.message.epoch == 3, timeout=5.0) is not None
    assert [e.message.epoch for e in listener.events] == [1, 2, 3]
    sock.close()


def test_mark_and_since_consider_only_later_events(listener):
    sock = _component(listener)
    sock.sendall(_role_changed(listener, listener.messages.LeadershipRole.Leader, 1))
    assert listener.wait_for(lambda e: True, timeout=5.0) is not None
    since = listener.mark()
    assert listener.wait_for(test_event_listener.leads(listener.messages), timeout=0.2, since=since) is None
    sock.close()


def test_a_message_id_that_is_not_a_test_event_fails_the_test(listener):
    sock = _component(listener)
    sock.sendall(pdu_framing.encode_pdu(999, 1, b"\x00"))
    with pytest.raises(test_event_listener.TestEventFailure, match="999"):
        listener.wait_for(lambda e: False, timeout=2.0)
    sock.close()


def test_a_wrong_canary_fails_the_test(listener):
    sock = _component(listener)
    frame = bytearray(_role_changed(listener, listener.messages.LeadershipRole.Leader))
    frame[16] ^= 0xFF
    sock.sendall(bytes(frame))
    with pytest.raises(test_event_listener.TestEventFailure, match="canary"):
        listener.wait_for(lambda e: False, timeout=2.0)
    sock.close()


def test_a_connection_closing_while_running_fails_the_test_but_not_when_stopped_on_purpose(listener):
    sock = _component(listener, "sequencer_secondary")
    listener.mark()
    sock.close()
    with pytest.raises(test_event_listener.TestEventFailure, match="sequencer_secondary"):
        listener.wait_for(lambda e: False, timeout=2.0)
    second = test_event_listener.TestEventListener(_PYTHON_DIR)
    sock = _component(second, "sequencer_secondary")
    second.mark()
    second.stopping("sequencer_secondary")
    sock.close()
    assert second.wait_for(lambda e: False, timeout=0.5) is None
    second.close()


def _answer_checkpoints(sock, events, stop):
    """Plays a component's thread: answers each checkpoint request with its number."""
    reader = pdu_framing.PduReader()
    sock.settimeout(0.1)
    while not stop.is_set():
        try:
            data = sock.recv(4096)
        except socket.timeout:
            continue
        if not data:
            return
        for header, body in reader.feed(data):
            if header.pdu_id == events.TestCheckpointRequest.MESSAGE_ID:
                request, _ = events.TestCheckpointRequest.decode(body)
                reached = events.TestCheckpointReached(number=request.number)
                sock.sendall(pdu_framing.encode_pdu(events.TestCheckpointReached.MESSAGE_ID, 1, reached.encode()))


def test_a_checkpoint_is_reached_once_the_component_answers(listener):
    sock = _component(listener)
    listener.mark()
    stop = threading.Event()
    answering = threading.Thread(target=_answer_checkpoints, args=(sock, listener.messages, stop))
    answering.start()
    try:
        assert listener.checkpoint("sequencer_primary", timeout=5.0)
    finally:
        stop.set()
        answering.join()
        sock.close()


def test_a_checkpoint_not_answered_fails(listener):
    sock = _component(listener)
    listener.mark()
    started = time.monotonic()
    assert not listener.checkpoint("sequencer_primary", timeout=0.5)
    assert time.monotonic() - started >= 0.5
    assert "did not answer" in listener.failures[-1]
    sock.close()


def test_a_checkpoint_of_a_component_with_no_connection_fails(listener):
    assert not listener.checkpoint("sequencer_primary", timeout=0.5)
    assert "no open test event connection" in listener.failures[-1]
