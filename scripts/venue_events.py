#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Starting the venue for a load or profiling run with test events, and learning from them when it is ready.

perf_run.py and callgrind_run.py start every component themselves. Each is started from a copy of its configuration
with a [test_events] section naming a listener this module opens, so that the components say, as test events, which
instance leads each pair and when a gateway is connected; the design is docs/availability/test_events.md. The scripts
wait on those events rather than on the wording of Quill messages, and accept whichever instance of a pair leads,
because either may win the first election.

Only the events a component sends of its own accord are used: roles, connections and the progress a gateway reports
every thousand orders. No component is asked to report each order, so a profiled run does no work for each order that
an ordinary run does not. The counts of orders come from the components' counters instead.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Callable, Optional

from test_event_listener import ReceivedEvent, TestEventListener, connection_made, leads

# The two instances of each pair, as the scripts name them and as the copies of their configurations name them.
SEQUENCERS = ("sequencer_primary", "sequencer_secondary")
ARBITERS = ("arbiter_primary", "arbiter_secondary")
MATCHING_ENGINES = ("matching_engine_primary", "matching_engine_secondary")


def start_listener(prefix: Path) -> TestEventListener:
    """Opens the listener the components connect to, decoding with the generated modules the release installs."""
    return TestEventListener(prefix / "lib" / "python")


def with_test_events(config: Path, listener: TestEventListener, name: str) -> Path:
    """A copy of a component's configuration with a [test_events] section naming the listener, and the name the script
    knows the component by, which its threads give when they introduce themselves.

    Written beside the original, so that the paths in it still resolve, and named after the component, because two
    components can be started from one file. Raises ValueError if the configuration already has such a section."""
    text = config.read_text()
    if "[test_events]" in text:
        raise ValueError(f"{config} already has a [test_events] section")
    text = text.rstrip("\n") + f"\n\n[test_events]\nhost = \"{listener.host}\"\nport = {listener.port}\ncomponent = \"{name}\"\n"
    copy = config.with_name(f"{name}_test_events.toml")
    copy.write_text(text)
    return copy


def _leads_one_of(listener: TestEventListener, components: tuple) -> Callable[[ReceivedEvent], bool]:
    """A predicate: one of components says it leads."""
    says_it_leads = leads(listener.messages)
    return lambda event: event.component in components and says_it_leads(event)


def wait_for_leader(listener: TestEventListener, components: tuple, timeout: float) -> Optional[str]:
    """Waits until one of the instances of a pair says it leads, and returns its name, or None if neither does within
    timeout seconds."""
    found = listener.wait_for(_leads_one_of(listener, components), timeout=timeout)
    return found.component if found is not None else None


def wait_for_gateway_connected(listener: TestEventListener, gateway: str, timeout: float) -> bool:
    """Waits until the gateway has made a connection to either sequencer, which it does only once its reactor runs, and
    returns whether it did within timeout seconds."""
    events = listener.messages
    to_sequencer = [connection_made(events, gateway, sequencer) for sequencer in SEQUENCERS]
    return listener.wait_for(lambda event: any(made(event) for made in to_sequencer), timeout=timeout) is not None


def report_service_of(gateway: str) -> str:
    """The name the sequencer gives its connection for reports to a gateway: gateway_<protocol>_<instance>, where the
    protocol is 1 for FIX and 2 for binary, and instance a is 1 and b is 2, as the sequencer's [[gateway]] entries
    number them."""
    protocol = 2 if gateway.startswith("binary_") else 1
    instance = {"a": 1, "b": 2}[gateway.rsplit("_", 1)[1]]
    return f"gateway_{protocol}_{instance}"


def wait_for_reports_route(listener: TestEventListener, sequencer: str, gateway: str, timeout: float) -> bool:
    """Waits until the sequencer has made its connection for reports to the gateway, and returns whether it did within
    timeout seconds. Until it has, the sequencer holds the reports for that gateway's members and sends them once the
    connection is made, so orders sent earlier are answered late, and a run that measures latency would count the wait."""
    return listener.wait_for(connection_made(listener.messages, sequencer, report_service_of(gateway)), timeout=timeout) is not None


class Leaders:
    """The instance of each pair that leads once the venue is ready, as the scripts name them."""

    def __init__(self, sequencer: str, arbiter: str, matching_engine: str) -> None:
        self.sequencer = sequencer
        self.arbiter = arbiter
        self.matching_engine = matching_engine


def wait_until_ready(listener: TestEventListener, gateway: str, timeout: float, log: Callable[[str], None]) -> Leaders:
    """Waits until a sequencer, an arbiter and a matching engine each lead, whichever instance it is, the gateway the run
    sends its orders through is connected to a sequencer, and the leading sequencer is connected to the gateway to send
    it reports. Returns which instances lead. Raises TimeoutError, naming what did not happen, if any of it does not
    happen within timeout seconds of the call."""
    deadline = time.monotonic() + timeout
    found = {}
    for label, pair in (("sequencer", SEQUENCERS), ("arbiter", ARBITERS), ("matching engine", MATCHING_ENGINES)):
        started = time.monotonic()
        leader = wait_for_leader(listener, pair, max(0.0, deadline - time.monotonic()))
        if leader is None:
            raise TimeoutError(f"no {label} said it leads within {timeout:.0f}s of the venue starting ({' or '.join(pair)})")
        log(f"  {leader} leads ({time.monotonic() - started:.1f}s)")
        found[label] = leader
    started = time.monotonic()
    if not wait_for_gateway_connected(listener, gateway, max(0.0, deadline - time.monotonic())):
        raise TimeoutError(f"{gateway} did not connect to a sequencer within {timeout:.0f}s of the venue starting")
    log(f"  {gateway} is connected to a sequencer ({time.monotonic() - started:.1f}s)")
    started = time.monotonic()
    if not wait_for_reports_route(listener, found["sequencer"], gateway, max(0.0, deadline - time.monotonic())):
        raise TimeoutError(f"{found['sequencer']} did not connect to {gateway} to send it reports within {timeout:.0f}s of the venue starting")
    log(f"  {found['sequencer']} is connected to {gateway} to send it reports ({time.monotonic() - started:.1f}s)")
    return Leaders(found["sequencer"], found["arbiter"], found["matching engine"])


def keep_reading(listener: TestEventListener, seconds: float) -> None:
    """Reads what the components send for seconds, and returns once they have passed, so that it serves both as a pause
    and as a way to keep the components' connections to the listener from backing up while a run waits on something
    else. A component goes on sending a few events during a run, such as a gateway's progress every thousand orders.

    The listener's pump returns as soon as anything arrives, so it is called until the time is up."""
    deadline = time.monotonic() + seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        listener.pump(remaining)
