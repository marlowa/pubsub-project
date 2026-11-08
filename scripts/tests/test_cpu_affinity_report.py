"""Tests for CPU affinity data and the graphical display model."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import sys
from unittest.mock import Mock

import pytest

SCRIPTS_DIRECTORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import cpu_affinity_report  # noqa: E402


def test_isolcpus_flags_are_distinguished():
    settings = cpu_affinity_report.parse_isolation_settings(
        "quiet isolcpus=domain,managed_irq,2-3 isolcpus=nohz,4-5 nohz_full=6-7"
    )

    assert settings["scheduler"] == {2, 3}
    assert settings["managed_irq"] == {2, 3}
    assert settings["isolcpus_nohz"] == {4, 5}
    assert settings["nohz_full"] == {6, 7}


def test_isolcpus_without_flags_defaults_to_scheduler_isolation():
    settings = cpu_affinity_report.parse_isolation_settings("isolcpus=2-3")

    assert settings["scheduler"] == {2, 3}


def test_managed_irq_only_does_not_mark_cpus_scheduler_isolated():
    settings = cpu_affinity_report.parse_isolation_settings("isolcpus=managed_irq,2-3")

    assert settings["scheduler"] == set()
    assert settings["managed_irq"] == {2, 3}


def test_core_types_use_cpu_capacity_then_cppc_performance():
    core_types = cpu_affinity_report.classify_cpu_core_types(
        [0, 1, 2, 3, 4],
        {0: 1024, 1: 512},
        {2: 100, 3: 79, 4: 0},
    )

    assert core_types == {
        0: "P-core",
        1: "E-core",
        2: "P-core",
        3: "E-core",
        4: "E-core",
    }


def test_core_type_is_unknown_when_linux_publishes_no_classification():
    core_types = cpu_affinity_report.classify_cpu_core_types([0], {}, {})

    assert core_types == {0: "Unknown"}


def test_unknown_core_type_has_no_display_label():
    label = cpu_affinity_report.CpuAffinityWindow._core_type_label("Unknown")

    assert label == ""


def test_summary_option_accepts_a_filename(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["cpu_affinity_report.py", "--summary", "summary.txt"])

    arguments = cpu_affinity_report.parse_arguments()

    assert arguments.summary == Path("summary.txt")
    assert not arguments.graphic


def test_graphic_and_summary_may_be_chosen_together(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["cpu_affinity_report.py", "--graphic", "--summary", "summary.txt"])

    arguments = cpu_affinity_report.parse_arguments()

    assert arguments.graphic
    assert arguments.summary == Path("summary.txt")


def test_an_output_must_be_chosen(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["cpu_affinity_report.py"])

    with pytest.raises(SystemExit) as stopped:
        cpu_affinity_report.parse_arguments()

    assert stopped.value.code != 0
    assert "--graphic" in capsys.readouterr().err


def test_summary_alone_is_written_without_tk(monkeypatch, tmp_path):
    # Tk cannot be imported, as on a machine with no graphics: the summary must still be written.
    monkeypatch.setitem(sys.modules, "tkinter", None)
    summary = tmp_path / "summary.txt"
    monkeypatch.setattr(sys, "argv", ["cpu_affinity_report.py", "--summary", str(summary), "--sample-seconds", "0.2"])

    status = cpu_affinity_report.main()

    assert status == 0
    text = summary.read_text(encoding="utf-8")
    assert text.startswith("CPU affinity summary\n")
    assert "CPU 0" in text


def test_text_summary_includes_cpu_and_thread_details():
    cards = cpu_affinity_report.build_cpu_cards(
        [
            {
                "pid": 42,
                "name": "risk_worker",
                "threads": [
                    {"thread_id": 43, "name": "risk", "processors": [0]},
                ],
            },
        ],
        [0, 1],
        {0: 25.0, 1: 0.0},
        None,
        {0: "Unknown", 1: "E-core"},
    )
    snapshot = cpu_affinity_report.SystemSnapshot(
        cards=cards,
        process_count=1,
        thread_count=1,
        unreadable_process_count=0,
        unreadable_thread_count=0,
        affinity_scan_complete=True,
        sample_seconds=1.0,
        updated_at="10:00:00",
    )

    summary = cpu_affinity_report.format_system_summary(snapshot)

    assert "CPU 0\n" in summary
    assert "CPU 0 (UNKNOWN)" not in summary
    assert "CPU 1 (E-CORE)" in summary
    assert "CPU usage: 25.0%" in summary
    assert "risk_worker / risk" in summary
    assert "PID 42 | TID 43 | CPUs 0" in summary


def test_summary_waits_for_usage_measurement_from_every_cpu():
    cards = cpu_affinity_report.build_cpu_cards(
        [],
        [0, 1],
        {},
        None,
    )
    snapshot = cpu_affinity_report.SystemSnapshot(
        cards=cards,
        process_count=0,
        thread_count=0,
        unreadable_process_count=0,
        unreadable_thread_count=0,
        affinity_scan_complete=True,
        sample_seconds=0.0,
        updated_at="10:00:00",
    )
    summary_path = Mock(spec=Path)

    written = cpu_affinity_report._write_summary_when_ready(summary_path, snapshot)

    assert not written
    summary_path.write_text.assert_not_called()

    snapshot = replace(snapshot, sample_seconds=1.0)
    cards[0].usage = 25.0
    written = cpu_affinity_report._write_summary_when_ready(summary_path, snapshot)

    assert not written
    summary_path.write_text.assert_not_called()

    cards[1].usage = 0.0
    written = cpu_affinity_report._write_summary_when_ready(summary_path, snapshot)

    assert written
    summary_path.write_text.assert_called_once_with(
        cpu_affinity_report.format_system_summary(snapshot),
        encoding="utf-8",
    )


def test_cpu_usage_is_calculated_from_busy_and_total_tick_deltas():
    before = {0: (50, 100), 1: (20, 100)}
    after = {0: (80, 200), 1: (70, 200)}

    usage = cpu_affinity_report.calculate_cpu_usage(before, after)

    assert usage == {0: 30.0, 1: 50.0}


def test_cpu_usage_is_unavailable_without_a_valid_interval():
    before = {0: (50, 100), 1: (20, 100)}
    after = {0: (50, 100), 1: (10, 90)}

    usage = cpu_affinity_report.calculate_cpu_usage(before, after)

    assert usage == {0: None, 1: None}


def test_thread_status_supplies_name_and_affinity_together():
    status = (
        "Name:\tmatching worker\n"
        "State:\tS (sleeping)\n"
        "Cpus_allowed_list:\t0-3,8\n"
    )

    details = cpu_affinity_report._parse_thread_status(status)

    assert details == ("matching worker", [0, 1, 2, 3, 8])


def test_thread_status_without_affinity_is_unreadable():
    details = cpu_affinity_report._parse_thread_status("Name:\tworker\n")

    assert details is None


def test_thread_rows_are_formatted_as_text_with_category_ranges():
    threads = [
        cpu_affinity_report.AffinityThread(
            "risk_worker", "risk", 42, 43, (2,), "single"
        ),
        cpu_affinity_report.AffinityThread(
            "gateway", "io", 50, 51, (0, 1, 2), "few"
        ),
    ]

    text, colour_ranges = cpu_affinity_report._format_thread_rows(threads)

    assert text == (
        "risk_worker / risk\n"
        "PID 42 | TID 43 | CPUs 2\n"
        "gateway / io\n"
        "PID 50 | TID 51 | CPUs 0-2\n"
    )
    assert colour_ranges == [("single", 1, 3), ("few", 3, 5)]


def test_cpu_cards_separate_single_cpu_threads_from_flexible_threads():
    processes = [
        {
            "pid": 42,
            "name": "risk_worker",
            "threads": [
                {"thread_id": 43, "name": "risk_worker", "processors": [3]},
                {"thread_id": 44, "name": "worker", "processors": [0, 1]},
                {"thread_id": 45, "name": "pool", "processors": [0, 1, 2, 3]},
            ],
        },
    ]
    settings = {
        "scheduler": {3},
        "managed_irq": set(),
        "isolcpus_nohz": {3},
        "nohz_full": {2},
    }

    cards = cpu_affinity_report.build_cpu_cards(
        processes,
        [0, 1, 2, 3],
        {0: 10.0, 1: 25.0, 2: 50.0, 3: 100.0},
        settings,
        {0: "P-core", 1: "E-core", 2: "Unknown", 3: "P-core"},
    )

    assert [card.processor for card in cards] == [0, 1, 2, 3]
    assert [card.core_type for card in cards] == ["P-core", "E-core", "Unknown", "P-core"]
    assert cards[0].usage == 10.0
    assert cards[0].multi_cpu_thread_count == 1
    assert cards[0].all_online_thread_count == 1
    assert cards[0].all_online_process_count == 1
    assert cards[2].nohz_full
    assert cards[3].scheduler_isolated
    assert cards[3].nohz_full
    assert cards[3].single_cpu_thread_count == 1


def test_threads_with_all_online_affinity_are_counted_not_repeated_as_rows():
    cards = cpu_affinity_report.build_cpu_cards(
        [
            {
                "pid": 42,
                "name": "worker_pool",
                "threads": [
                    {"thread_id": 43, "name": "worker", "processors": [0, 1, 2, 3]},
                ],
            },
        ],
        [0, 1, 2, 3],
        {},
        None,
    )

    assert [card.all_online_thread_count for card in cards] == [1, 1, 1, 1]
    assert all(card.threads == [] for card in cards)
