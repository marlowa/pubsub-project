"""Tests for waits in ha_test.py."""

from pathlib import Path
import sys

import pytest

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIR))

import ha_test  # noqa: E402


class ExitedProcess:
    def __init__(self) -> None:
        self.pid = 1
        self.poll_count = 0

    def poll(self) -> int:
        self.poll_count += 1
        return 1


@pytest.mark.parametrize("wait_function", [ha_test.await_new_orders, ha_test.await_new_orders_since])
def test_order_wait_stops_when_the_client_exits(monkeypatch, wait_function):
    process = ExitedProcess()
    monkeypatch.setitem(ha_test._CLIENT_REPORTS, process.pid, ha_test.ClientReports(Path("not-created.txt")))

    def unexpected_sleep(_seconds: float) -> None:
        pytest.fail("the wait slept after the client exited")

    monkeypatch.setattr(ha_test.time, "sleep", unexpected_sleep)

    found, _elapsed = wait_function(process, 1, 0, timeout=60.0)

    assert not found
    assert process.poll_count == 1
