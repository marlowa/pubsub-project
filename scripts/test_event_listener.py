#!/usr/bin/env python3
"""The high availability tests' end of the test event channel (docs/availability/test_events.md).

The test opens a TestEventListener before it starts the venue, and passes its address to each component that is to
send test events, in a [test_events] section of the configuration the component is started with. Each such component
connects, says which component, instance and thread it is (TestEventsHello), and sends its events as framework PDUs:
DSL messages from libraries/pubsub_itc_fw/include/pubsub_itc_fw/test_events.dsl, and from the DSL files applications keep
for their own events (EVENT_MODULES), decoded here with the Python modules the build generates from those files and
installs into lib/python, beside the PDU framing (python/dsl/pdu_framing.py).

The listener reads its sockets only while the test waits for an event or a checkpoint. There is no thread of its own:
whatever arrives meanwhile waits in the kernel's buffers and the component's send queue, which hold far more than the
few events a scenario produces.

Events from one connection keep the order they were sent in; there is no order between connections. A test that checks
that something did NOT happen asks the component for a checkpoint first: the component answers once it has dealt with
everything before the request, so every event it sent before the answer has arrived when the answer does.

Anything that would weaken a test fails it instead: a message that does not decode, a message id the DSL does not
define, a framing error, or a component's connection closing while its process goes on running. All of these are
collected in `failures`, and check() raises on the first.

A connection closes, and nothing is wrong, whenever the test stops or kills a component, and also when a supervisor
restarts one. So the listener does not need to be told which components the test is about to stop. The introduction
carries the component's process id, and when the connection closes the listener looks at that process. If the process
has ended, the close is expected. If it is still running a few seconds later (close_grace), the component lost its
connection to the test while it carried on, so events it reported afterwards were never seen, and the test fails.
The few seconds allow for a component that closes its connections as it shuts down and then takes a moment to exit.
"""

from __future__ import annotations

import importlib
import selectors
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional


# The generated modules whose messages may arrive on a test event connection: the library's first, then each application's.
EVENT_MODULES = ("test_events", "matching_engine_test_events", "sequencer_test_events", "fix_order_gateway_test_events")


def load_generated_modules(python_dir: Path, module_names=EVENT_MODULES):
    """Import the generated event modules and pdu_framing from the installed lib/python directory.

    Returns the modules by name, and pdu_framing. Two modules that define the same message id would make a message
    ambiguous, so that is refused here rather than decoded as whichever came first."""
    if str(python_dir) not in sys.path:
        sys.path.insert(0, str(python_dir))
    modules = {name: importlib.import_module(name) for name in module_names}
    owner: Dict[int, str] = {}
    for name, module in modules.items():
        for message_id in module.MESSAGES_BY_ID:
            if message_id in owner:
                raise TestEventFailure(f"message id {message_id} is defined by both {owner[message_id]} and {name}")
            owner[message_id] = name
    return modules, importlib.import_module("pdu_framing")


class TestEventFailure(AssertionError):
    """Something on the test event channel that would make a test's conclusion untrustworthy."""


@dataclass
class ReceivedEvent:  # pylint: disable=too-many-instance-attributes
    """One event as it arrived: which connection, which component sent it, and the decoded message."""

    index: int
    arrived: float
    connection: int
    component: str
    instance: int
    thread: str
    message: object


@dataclass
class _Connection:  # pylint: disable=too-many-instance-attributes
    number: int
    sock: socket.socket
    reader: object
    component: str = ""
    instance: int = 0
    thread: str = ""
    process_id: int = 0
    open: bool = True
    closed_at: float = 0.0
    how_closed: str = ""
    checkpoints: Dict[int, bool] = field(default_factory=dict)


class TestEventListener:  # pylint: disable=too-many-instance-attributes
    """Accepts components' test event connections on the loopback interface and keeps what they send."""

    def __init__(self, python_dir: Path, close_grace: float = 5.0, module_names=EVENT_MODULES) -> None:
        self.modules, self._framing = load_generated_modules(python_dir, module_names)
        self._events_module = self.modules["test_events"]
        self._messages_by_id: Dict[int, type] = {}
        self._decode_errors = tuple({module.DecodeError for module in self.modules.values()})
        for module in self.modules.values():
            self._messages_by_id.update(module.MESSAGES_BY_ID)
        self._selector = selectors.DefaultSelector()
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind(("127.0.0.1", 0))
        self._server.listen(32)
        self._server.setblocking(False)
        self._selector.register(self._server, selectors.EVENT_READ, None)
        self._connections: Dict[int, _Connection] = {}
        self._next_connection = 1
        self._next_checkpoint = 1
        self.events: List[ReceivedEvent] = []
        self.failures: List[str] = []
        # How long a component's process may go on running after its connection closed before the test fails.
        self._close_grace = close_grace
        # Connections that have closed, whose process has not yet been seen to end.
        self._closed_awaiting_exit: List[_Connection] = []

    @property
    def messages(self):
        """The generated test_events module, for its message classes and LeadershipRole."""
        return self._events_module

    @property
    def host(self) -> str:
        """The address the listener accepts connections on: the loopback interface."""
        return "127.0.0.1"

    @property
    def port(self) -> int:
        """The port the operating system chose for the listener."""
        return self._server.getsockname()[1]

    def close(self) -> None:
        """Close every connection and stop listening."""
        for connection in self._connections.values():
            if connection.open:
                self._selector.unregister(connection.sock)
                connection.sock.close()
        self._selector.unregister(self._server)
        self._server.close()

    # ------------------------------------------------------------------------------------------------------------
    # Reading
    # ------------------------------------------------------------------------------------------------------------

    def pump(self, timeout: float = 0.0) -> None:
        """Read whatever has arrived, waiting up to timeout seconds for something to."""
        for key, _ in self._selector.select(timeout):
            if key.data is None:
                self._accept()
            else:
                self._read(key.data)

    def _accept(self) -> None:
        sock, _ = self._server.accept()
        sock.setblocking(False)
        connection = _Connection(number=self._next_connection, sock=sock, reader=self._framing.PduReader())
        self._next_connection += 1
        self._connections[connection.number] = connection
        self._selector.register(sock, selectors.EVENT_READ, connection)

    def _read(self, connection: _Connection) -> None:
        try:
            data = connection.sock.recv(65536)
        except ConnectionError as error:
            self._note_closed(connection, f"connection error: {error}")
            return
        if not data:
            self._note_closed(connection, "closed")
            return
        try:
            pdus = connection.reader.feed(data)
        except self._framing.FramingError as error:
            self.failures.append(f"test events from {self._who(connection)}: {error}")
            return
        for header, body in pdus:
            self._handle(connection, header, body)

    def _handle(self, connection: _Connection, header, body: bytes) -> None:
        message_class = self._messages_by_id.get(header.pdu_id)
        if message_class is None:
            self.failures.append(f"test events from {self._who(connection)}: message id {header.pdu_id} is not a test event")
            return
        try:
            message, end = message_class.decode(body)
        except ValueError as error:  # each generated module's DecodeError is a ValueError
            if not isinstance(error, self._decode_errors):
                raise
            self.failures.append(f"test events from {self._who(connection)}: {error}")
            return
        if end != len(body):
            self.failures.append(f"test events from {self._who(connection)}: {len(body) - end} byte(s) after a {message_class.__name__}")
            return
        if isinstance(message, self._events_module.TestEventsHello):
            connection.component, connection.instance, connection.thread = message.component, message.instance, message.thread
            connection.process_id = message.process_id
            # Kept among the events as well, so that a test can wait for a component to start, and can look only at what
            # the component's present process reported (since_introduction).
        elif isinstance(message, self._events_module.TestCheckpointReached):
            connection.checkpoints[message.number] = True
            return
        self.events.append(ReceivedEvent(index=len(self.events), arrived=time.monotonic(), connection=connection.number,
                                         component=connection.component, instance=connection.instance, thread=connection.thread,
                                         message=message))

    def _note_closed(self, connection: _Connection, how: str) -> None:
        connection.open = False
        connection.closed_at = time.monotonic()
        connection.how_closed = how
        self._selector.unregister(connection.sock)
        connection.sock.close()
        # A connection that closed before introducing itself had sent no events, so nothing was lost.
        if connection.process_id != 0:
            self._closed_awaiting_exit.append(connection)
        self._judge_closed_connections()

    def _judge_closed_connections(self) -> None:
        """Fail the test for each closed connection whose process is still running close_grace seconds after the close."""
        still_waiting = []
        for connection in self._closed_awaiting_exit:
            if not process_is_running(connection.process_id):
                continue
            if time.monotonic() - connection.closed_at < self._close_grace:
                still_waiting.append(connection)
                continue
            self.failures.append(f"the test event connection from {self._who(connection)} {connection.how_closed}, "
                                 f"and its process {connection.process_id} was still running {self._close_grace:.0f}s later")
        self._closed_awaiting_exit = still_waiting

    @staticmethod
    def _who(connection: _Connection) -> str:
        return f"{connection.component or 'an unnamed component'} ({connection.thread or 'thread not yet named'})"

    # ------------------------------------------------------------------------------------------------------------
    # What a test uses
    # ------------------------------------------------------------------------------------------------------------

    def check(self) -> None:
        """Raise TestEventFailure if anything has gone wrong on the channel."""
        self.pump()
        self._judge_closed_connections()
        if self.failures:
            raise TestEventFailure(self.failures[0])

    def mark(self) -> int:
        """A position in the events: pass it as since to consider only the events that arrive after now."""
        self.pump()
        return len(self.events)

    def since_introduction(self, component: str) -> int:
        """The position of the newest introduction from component, so that only its present process's events are considered.

        A test that looks for what a component has done since it started, rather than since some step of the test,
        passes this as since. When component has not introduced itself, every event is considered.
        """
        self.pump()
        for event in reversed(self.events):
            if event.component == component and isinstance(event.message, self._events_module.TestEventsHello):
                return event.index
        return 0

    def has_open_connection(self, component: str) -> bool:
        """Whether component has a test event connection open now."""
        self.pump()
        return any(c.open and c.component == component for c in self._connections.values())

    def settle(self, component: str, timeout: float = 10.0) -> bool:
        """Make sure every event component has sent so far has been read, before a test checks that something is absent.

        With a connection open, this is a checkpoint. With none, the component's process has gone, and its
        connection was read to the end before the close was seen, so everything it sent has already arrived.
        """
        if self.has_open_connection(component):
            return self.checkpoint(component, timeout)
        return True

    def find(self, predicate: Callable[[ReceivedEvent], bool], since: int = 0) -> Optional[ReceivedEvent]:
        """The first event at or after since for which predicate holds, among those already read; or None."""
        for event in self.events[since:]:
            if predicate(event):
                return event
        return None

    def wait_for(self, predicate: Callable[[ReceivedEvent], bool], timeout: float, since: int = 0) -> Optional[ReceivedEvent]:
        """The first event at or after since for which predicate holds, waiting up to timeout seconds; or None."""
        deadline = time.monotonic() + timeout
        while True:
            self.check()
            found = self.find(predicate, since)
            if found is not None:
                return found
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.pump(min(remaining, 0.05))

    def checkpoint(self, component: str, timeout: float = 10.0) -> bool:
        """Ask every open connection from component for a checkpoint, and wait until each has answered.

        Once this returns True, every event those connections sent before answering has been read.
        """
        targets = [c for c in self._connections.values() if c.open and c.component == component]
        if not targets:
            self.pump(0.05)
            targets = [c for c in self._connections.values() if c.open and c.component == component]
        if not targets:
            self.failures.append(f"checkpoint: no open test event connection from {component}")
            return False
        number = self._next_checkpoint
        self._next_checkpoint += 1
        request = self._events_module.TestCheckpointRequest(number=number).encode()
        frame = self._framing.encode_pdu(self._events_module.TestCheckpointRequest.MESSAGE_ID, 1, request)
        for connection in targets:
            connection.sock.setblocking(True)
            connection.sock.sendall(frame)
            connection.sock.setblocking(False)
        deadline = time.monotonic() + timeout
        while not all(c.checkpoints.get(number) for c in targets):
            self.check()
            if time.monotonic() >= deadline:
                self.failures.append(f"checkpoint: {component} did not answer within {timeout:.0f}s")
                return False
            self.pump(0.05)
        return True


def process_is_running(process_id: int) -> bool:
    """Whether process_id names a process that has not ended.

    A process that has ended but has not yet been waited for by its parent (a zombie) has ended: it runs no code and
    holds no sockets. The state is the third field of /proc/<pid>/stat, after the command name in parentheses, which
    may itself contain spaces and parentheses, hence the search from the right.
    """
    try:
        stat = Path(f"/proc/{process_id}/stat").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    state = stat[stat.rfind(")") + 1:].split()[0]
    return state not in ("Z", "X")


def leads(test_events_module, component: Optional[str] = None) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a RoleChanged saying that the instance now leads, from component if one is named."""
    return role_changed(test_events_module, component, test_events_module.LeadershipRole.Leader)


def follows(test_events_module, component: Optional[str] = None) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a RoleChanged saying that the instance now follows, from component if one is named."""
    return role_changed(test_events_module, component, test_events_module.LeadershipRole.Follower)


def role_changed(test_events_module, component: Optional[str] = None, to_role=None) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a RoleChanged, to to_role if one is given, from component if one is named."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, test_events_module.RoleChanged) and (to_role is None or event.message.to_role == to_role)
                and (component is None or event.component == component))
    return predicate


def lease_changed(test_events_module, component: Optional[str], change, granted_by=None) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a LeaseChanged of the given kind, by the given voter if one is given, from component if one is named."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, test_events_module.LeaseChanged) and event.message.change == change
                and (granted_by is None or event.message.granted_by == granted_by) and (component is None or event.component == component))
    return predicate


def granted_to(test_events_module, component: Optional[str], pair_id: int, instance: int) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a LeaseGrantedTo saying that a third voter (component, if named) granted instance of pair_id a lease."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, test_events_module.LeaseGrantedTo) and event.message.pair_id == pair_id
                and event.message.instance == instance and (component is None or event.component == component))
    return predicate


def introduced(test_events_module, component: Optional[str] = None) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a thread of component (any component, if none is named) introduced itself: it has started."""
    def predicate(event: ReceivedEvent) -> bool:
        return isinstance(event.message, test_events_module.TestEventsHello) and (component is None or event.component == component)
    return predicate


def order_outcome(engine_events_module, component: Optional[str], cl_ord_id: str, outcome=None,
                  during_catch_up: Optional[bool] = None) -> Callable[[ReceivedEvent], bool]:
    """A predicate: an OrderOutcome from the matching engine (component, if named) for cl_ord_id, with the given outcome
    and catch-up flag where those are given."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, engine_events_module.OrderOutcome) and event.message.cl_ord_id == cl_ord_id
                and (outcome is None or event.message.outcome == outcome)
                and (during_catch_up is None or bool(event.message.during_catch_up) == during_catch_up)
                and (component is None or event.component == component))
    return predicate


def message_of(message_class, component: Optional[str] = None, **fields) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a message of message_class, from component if one is named, whose fields equal those given."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, message_class) and (component is None or event.component == component)
                and all(getattr(event.message, name) == value for name, value in fields.items()))
    return predicate


def catch_up(engine_events_module, component: Optional[str], stage) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a CatchUp at the given stage from the matching engine (component, if named)."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, engine_events_module.CatchUp) and event.message.stage == stage
                and (component is None or event.component == component))
    return predicate


def connection_made(test_events_module, component: Optional[str], service: str) -> Callable[[ReceivedEvent], bool]:
    """A predicate: component (any, if none is named) made a connection to service."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, test_events_module.ConnectionChanged) and event.message.made and event.message.service == service
                and (component is None or event.component == component))
    return predicate
