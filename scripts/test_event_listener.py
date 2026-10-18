#!/usr/bin/env python3
"""The high availability tests' end of the test event channel (docs/availability/test_events.md).

The test opens a TestEventListener before it starts the venue, and passes its address to each component that is to
send test events, in a [test_events] section of the configuration the component is started with. Each such component
connects, says which component, instance and thread it is (TestEventsHello), and sends its events as framework PDUs:
DSL messages from libraries/pubsub_itc_fw/include/pubsub_itc_fw/test_events.dsl, decoded here with the Python module
the build generates from that file and installs into lib/python, beside the PDU framing (python/dsl/pdu_framing.py).

The listener reads its sockets only while the test waits for an event or a checkpoint. There is no thread of its own:
whatever arrives meanwhile waits in the kernel's buffers and the component's send queue, which hold far more than the
few events a scenario produces.

Events from one connection keep the order they were sent in; there is no order between connections. A test that checks
that something did NOT happen asks the component for a checkpoint first: the component answers once it has dealt with
everything before the request, so every event it sent before the answer has arrived when the answer does.

Anything that would weaken a test fails it instead: a message that does not decode, a message id the DSL does not
define, a framing error, or a component's connection closing while the test expects it to be running. All of these
are collected in `failures`, and check() raises on the first.
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


def load_generated_modules(python_dir: Path):
    """Import the generated test_events module and pdu_framing from the installed lib/python directory."""
    if str(python_dir) not in sys.path:
        sys.path.insert(0, str(python_dir))
    return importlib.import_module("test_events"), importlib.import_module("pdu_framing")


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
    open: bool = True
    checkpoints: Dict[int, bool] = field(default_factory=dict)


class TestEventListener:  # pylint: disable=too-many-instance-attributes
    """Accepts components' test event connections on the loopback interface and keeps what they send."""

    def __init__(self, python_dir: Path) -> None:
        self._events_module, self._framing = load_generated_modules(python_dir)
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
        # Components whose connection may close without failing the test: those the test stopped on purpose.
        self._may_close: set = set()

    @property
    def messages(self):
        """The generated test_events module, for its message classes and LeadershipRole."""
        return self._events_module

    @property
    def host(self) -> str:
        return "127.0.0.1"

    @property
    def port(self) -> int:
        return self._server.getsockname()[1]

    def close(self) -> None:
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
        message_class = self._events_module.MESSAGES_BY_ID.get(header.pdu_id)
        if message_class is None:
            self.failures.append(f"test events from {self._who(connection)}: message id {header.pdu_id} is not a test event")
            return
        try:
            message, end = message_class.decode(body)
        except self._events_module.DecodeError as error:
            self.failures.append(f"test events from {self._who(connection)}: {error}")
            return
        if end != len(body):
            self.failures.append(f"test events from {self._who(connection)}: {len(body) - end} byte(s) after a {message_class.__name__}")
            return
        if isinstance(message, self._events_module.TestEventsHello):
            connection.component, connection.instance, connection.thread = message.component, message.instance, message.thread
            return
        if isinstance(message, self._events_module.TestCheckpointReached):
            connection.checkpoints[message.number] = True
            return
        self.events.append(ReceivedEvent(index=len(self.events), arrived=time.monotonic(), connection=connection.number,
                                         component=connection.component, instance=connection.instance, thread=connection.thread,
                                         message=message))

    def _note_closed(self, connection: _Connection, how: str) -> None:
        connection.open = False
        self._selector.unregister(connection.sock)
        connection.sock.close()
        if connection.component not in self._may_close:
            self.failures.append(f"the test event connection from {self._who(connection)} {how} while it was meant to be running")

    @staticmethod
    def _who(connection: _Connection) -> str:
        return f"{connection.component or 'an unnamed component'} ({connection.thread or 'thread not yet named'})"

    # ------------------------------------------------------------------------------------------------------------
    # What a test uses
    # ------------------------------------------------------------------------------------------------------------

    def check(self) -> None:
        """Raise TestEventFailure if anything has gone wrong on the channel."""
        self.pump()
        if self.failures:
            raise TestEventFailure(self.failures[0])

    def mark(self) -> int:
        """A position in the events: pass it as since to consider only the events that arrive after now."""
        self.pump()
        return len(self.events)

    def stopping(self, component: str) -> None:
        """Say that the test is about to stop component, so that its connection closing does not fail the test."""
        self._may_close.add(component)

    def started(self, component: str) -> None:
        """Say that component is running again, so that its connection closing fails the test again."""
        self._may_close.discard(component)

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


def leads(test_events_module, component: Optional[str] = None) -> Callable[[ReceivedEvent], bool]:
    """A predicate: a RoleChanged saying that the instance now leads, from component if one is named."""
    def predicate(event: ReceivedEvent) -> bool:
        return (isinstance(event.message, test_events_module.RoleChanged) and event.message.to_role == test_events_module.LeadershipRole.Leader
                and (component is None or event.component == component))
    return predicate
