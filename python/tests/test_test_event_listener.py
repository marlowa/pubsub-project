# pylint: disable=redefined-outer-name
"""Tests of the high availability tests' end of the test event channel (scripts/test_event_listener.py).

A component is played by a socket in the test, sending what the library's TestEventChannel sends. The generated
test_events module is made from the library's DSL file into the build tree, as the build makes it.
"""

import os
import socket
import subprocess
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
    """test_events.py and matching_engine_test_events.py, generated from their DSL files, beside a copy of pdu_framing.py."""
    _PYTHON_DIR.mkdir(parents=True, exist_ok=True)
    for dsl in (_PROJECT_ROOT / "libraries" / "pubsub_itc_fw" / "include" / "pubsub_itc_fw" / "test_events.dsl",
                _PROJECT_ROOT / "applications" / "matching_engine" / "matching_engine_test_events.dsl"):
        tree = load(dsl)
        Validator(tree).validate()
        (_PYTHON_DIR / f"{dsl.stem}.py").write_text(PythonGenerator(source_name=dsl.name).emit(tree))
    (_PYTHON_DIR / "pdu_framing.py").write_text(Path(pdu_framing.__file__).read_text(encoding="utf-8"))


@pytest.fixture
def listener():
    made = test_event_listener.TestEventListener(_PYTHON_DIR)
    yield made
    made.close()


def _component(listener, name="sequencer_primary", process_id=None):
    """A connected socket that has introduced itself as name's SequencerThread, running as process_id (this process if None)."""
    sock = socket.create_connection((listener.host, listener.port))
    events = listener.messages
    hello = events.TestEventsHello(component=name, instance=1, thread="SequencerThread", process_id=os.getpid() if process_id is None else process_id)
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
    assert listener.wait_for(lambda e: getattr(e.message, "epoch", 0) == 3, timeout=5.0) is not None
    assert [e.message.epoch for e in listener.events if isinstance(e.message, listener.messages.RoleChanged)] == [1, 2, 3]
    sock.close()


def test_mark_and_since_consider_only_later_events(listener):
    sock = _component(listener)
    sock.sendall(_role_changed(listener, listener.messages.LeadershipRole.Leader, 1))
    assert listener.wait_for(lambda e: True, timeout=5.0) is not None
    since = listener.mark()
    assert listener.wait_for(test_event_listener.leads(listener.messages), timeout=0.2, since=since) is None
    sock.close()


def test_an_introduction_is_kept_as_an_event_and_marks_where_the_present_process_began(listener):
    first = _component(listener)
    first.sendall(_role_changed(listener, listener.messages.LeadershipRole.Leader, 1))
    assert listener.wait_for(test_event_listener.leads(listener.messages, "sequencer_primary"), timeout=5.0) is not None
    assert listener.find(test_event_listener.introduced(listener.messages, "sequencer_primary")) is not None
    second = _component(listener)  # the component, started again
    second.sendall(_role_changed(listener, listener.messages.LeadershipRole.Follower, 2))
    assert listener.wait_for(test_event_listener.follows(listener.messages, "sequencer_primary"), timeout=5.0) is not None
    since = listener.since_introduction("sequencer_primary")
    assert listener.find(test_event_listener.leads(listener.messages, "sequencer_primary"), since=since) is None
    assert listener.find(test_event_listener.follows(listener.messages, "sequencer_primary"), since=since) is not None
    assert listener.since_introduction("nobody") == 0
    first.close()
    second.close()


def test_an_engine_order_outcome_arrives_decoded_with_its_sender(listener):
    sock = _component(listener, "matching_engine")
    engine = listener.modules["matching_engine_test_events"]
    outcome = engine.OrderOutcome(comp_id="INJECT-TEST", cl_ord_id="ord-1", outcome=engine.OrderOutcomeKind.RefusedTooLong,
                                  during_catch_up=True, seq_no=42)
    sock.sendall(pdu_framing.encode_pdu(engine.OrderOutcome.MESSAGE_ID, 1, outcome.encode()))
    refused = test_event_listener.order_outcome(engine, "matching_engine", "ord-1", engine.OrderOutcomeKind.RefusedTooLong, during_catch_up=True)
    found = listener.wait_for(refused, timeout=5.0)
    assert found is not None and found.message.seq_no == 42 and found.message.comp_id == "INJECT-TEST"
    assert listener.find(test_event_listener.order_outcome(engine, None, "ord-1", during_catch_up=False)) is None
    assert listener.find(test_event_listener.order_outcome(engine, None, "ord-2")) is None
    sock.close()


def test_two_event_modules_defining_one_message_id_are_refused():
    # The clash is made by loading the library's module twice under two names, in the build tree's test directory.
    (_PYTHON_DIR / "test_events_again.py").write_text((_PYTHON_DIR / "test_events.py").read_text())
    try:
        with pytest.raises(test_event_listener.TestEventFailure, match="defined by both test_events and test_events_again"):
            test_event_listener.TestEventListener(_PYTHON_DIR, module_names=("test_events", "test_events_again"))
    finally:
        (_PYTHON_DIR / "test_events_again.py").unlink()


def test_settling_a_component_with_no_connection_needs_no_checkpoint(listener):
    assert listener.settle("sequencer_primary", timeout=0.5)
    assert not listener.failures


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


def test_a_connection_closing_while_its_process_runs_on_fails_the_test():
    short_grace = test_event_listener.TestEventListener(_PYTHON_DIR, close_grace=0.3)
    try:
        sock = _component(short_grace, "sequencer_secondary")  # this test's own process, which goes on running
        short_grace.mark()
        sock.close()
        assert short_grace.wait_for(lambda e: False, timeout=0.1) is None  # within the grace period: not yet a failure
        with pytest.raises(test_event_listener.TestEventFailure, match="sequencer_secondary.*still running"):
            short_grace.wait_for(lambda e: False, timeout=2.0)
    finally:
        short_grace.close()


def test_a_connection_closing_because_its_process_ended_is_expected():
    short_grace = test_event_listener.TestEventListener(_PYTHON_DIR, close_grace=0.3)
    finished = subprocess.Popen(["true"])  # pylint: disable=consider-using-with
    finished.wait()
    try:
        sock = _component(short_grace, "sequencer_secondary", process_id=finished.pid)
        short_grace.mark()
        sock.close()
        assert short_grace.wait_for(lambda e: False, timeout=1.0) is None
    finally:
        short_grace.close()


def test_a_process_that_has_ended_but_not_been_waited_for_is_not_running():
    zombie = subprocess.Popen(["true"])  # pylint: disable=consider-using-with
    deadline = time.monotonic() + 5.0
    while test_event_listener.process_is_running(zombie.pid) and time.monotonic() < deadline:
        time.sleep(0.01)
    assert not test_event_listener.process_is_running(zombie.pid)
    assert test_event_listener.process_is_running(os.getpid())
    zombie.wait()


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
