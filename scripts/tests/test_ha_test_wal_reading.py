"""Tests for how ha_test.py reads a write-ahead log that a running sequencer may be changing."""

from pathlib import Path
import sys

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIR))

import ha_test  # noqa: E402


def test_a_segment_deleted_after_the_directory_was_listed_reads_as_empty(tmp_path: Path) -> None:
    # A symbolic link to nothing is listed by glob and cannot be read, as a segment deleted just after the listing is.
    (tmp_path / "wal_000040.log").symlink_to(tmp_path / "deleted")
    assert ha_test.wal_record_ids(tmp_path) == []
    assert ha_test.wal_records_after(tmp_path, 0) == {}
    assert list(ha_test.wal_entries(tmp_path)) == []
    assert ha_test.wal_problems(tmp_path)[2] == 0
