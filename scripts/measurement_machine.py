#!/usr/bin/env python3
"""
measurement_machine.py -- put this machine into a state where latency figures mean something,
take it back out again, and say which state it is in.

Two processor settings decide whether a latency measurement on this machine is worth anything.
Neither is the default and neither survives a reboot.

  The governor.        Left at the distribution's default the processors run far below what the
                       hardware allows, so every piece of work the venue does -- parsing,
                       matching, encoding -- takes several times longer than it needs to.

  The deep idle states. A core that has dropped into one costs a long time to leave: on this
                       machine 127 microseconds for one of them and over a millisecond for the
                       other. Every hop in the venue hands work to a thread that may be asleep,
                       so a measurement taken with these enabled is largely a measurement of
                       waking up.

Measured on this project on 2026-09-19, the two together were worth a factor of five to seven on
the round trip. Every figure taken before they were understood was wrong by that much, and the
error was silent: nothing failed, nothing logged, the numbers were simply untrue.

WHY THIS IS NOT LEFT SWITCHED ON. The settings exist to make measurements comparable, not because
the venue needs them. Disabling the deep idle states costs power and fan noise continuously, and
on a machine used for anything else that is a poor trade for most of its life. So this is meant to
be switched on for a measuring session and off afterwards, which is what --off is for.

WHAT --off RESTORES. Whatever was recorded when --on ran, which is kept in a file under /run and
therefore lasts exactly as long as the settings themselves -- both are gone after a reboot. Where
that file is missing, --off restores the documented defaults and says that is what it did.

Changing anything needs root. Reading does not, so --status works as any user and is what the
measurement tooling calls.

Usage:
  ./measurement_machine.py                 say which state the machine is in
  sudo ./measurement_machine.py --on       make measurements meaningful
  sudo ./measurement_machine.py --off      put the machine back
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

_CPU_BASE = Path("/sys/devices/system/cpu")

# An idle state costing longer than this to leave is switched off while measuring. The figure is
# not arbitrary: it is above the one microsecond the shallow states cost, which are worth keeping
# because leaving them is nearly free, and below the 127 microseconds of the cheapest state that
# is worth disabling. Anything in between would be a coin toss on this machine.
_IDLE_LATENCY_LIMIT_US = 100

# The governor a measuring machine wants, and the one the distribution leaves behind it.
_MEASURING_GOVERNOR = "performance"
_DEFAULT_GOVERNOR = "powersave"

# Where the state before --on is recorded. /run is cleared at boot, which is exactly the lifetime
# of the settings themselves: if the machine has rebooted, both the settings and this file are
# gone together and neither can mislead the other.
_SAVED_STATE = Path("/run/pubsub-measurement-machine.json")


def online_processors() -> list[int]:
    """Every processor the kernel currently has online."""
    processors: list[int] = []
    try:
        text = (_CPU_BASE / "online").read_text(encoding="utf-8").strip()
    except OSError:
        return list(range(os.cpu_count() or 1))
    for part in text.split(","):
        if "-" in part:
            first, _, last = part.partition("-")
            processors.extend(range(int(first), int(last) + 1))
        elif part:
            processors.append(int(part))
    return sorted(set(processors))


def read_value(path: Path) -> str:
    """One value from a sysfs file, or an empty string when it cannot be read."""
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def write_value(path: Path, value: str) -> bool:
    """Write one value to a sysfs file. False when it could not be written, which usually means
    this is not running as root."""
    try:
        path.write_text(value, encoding="utf-8")
        return True
    except OSError:
        return False


def governors() -> dict[int, str]:
    """The governor each processor is using."""
    found: dict[int, str] = {}
    for processor in online_processors():
        value = read_value(_CPU_BASE / f"cpu{processor}" / "cpufreq" / "scaling_governor")
        if value:
            found[processor] = value
    return found


def idle_states(processor: int) -> list[dict]:
    """Every idle state a processor offers, with how long it costs to leave and whether it is off."""
    states: list[dict] = []
    directory = _CPU_BASE / f"cpu{processor}" / "cpuidle"
    try:
        entries = sorted(entry for entry in directory.iterdir() if entry.name.startswith("state"))
    except OSError:
        return states
    for entry in entries:
        latency = read_value(entry / "latency")
        states.append({
            "state": entry.name,
            "name": read_value(entry / "name"),
            "latency_us": int(latency) if latency.isdigit() else -1,
            "disabled": read_value(entry / "disable") == "1",
        })
    return states


def describe_state() -> tuple[bool, list[str]]:
    """Whether this machine is in a state where a latency figure means something, and why not.

    Returns the verdict and a list of plain statements about what is wrong. An empty list means
    nothing is. This is what the measurement tooling calls, so it deliberately reads and never
    writes, and needs no privilege.
    """
    problems: list[str] = []

    using = governors()
    wrong = sorted(processor for processor, name in using.items() if name != _MEASURING_GOVERNOR)
    if not using:
        problems.append("no processor reports a frequency governor, so nothing can be said about clock speed")
    elif wrong:
        names = sorted({using[processor] for processor in wrong})
        problems.append(f"{len(wrong)} processor(s) are using the {'/'.join(names)} governor rather than {_MEASURING_GOVERNOR}, "
                        f"so they run below what the hardware allows")

    deep_and_enabled: dict[str, list[int]] = {}
    for processor in online_processors():
        for state in idle_states(processor):
            if state["latency_us"] > _IDLE_LATENCY_LIMIT_US and not state["disabled"]:
                deep_and_enabled.setdefault(f"{state['name']} ({state['latency_us']}us to leave)", []).append(processor)
    for description, processors in sorted(deep_and_enabled.items()):
        problems.append(f"idle state {description} is enabled on {len(processors)} processor(s), so a core that has been "
                        f"asleep costs that long to wake")

    return not problems, problems


def save_current_state() -> None:
    """Record what to put back, so --off restores what was really there rather than a guess."""
    recorded = {
        "governors": {str(processor): name for processor, name in governors().items()},
        "disabled_idle_states": {
            str(processor): [state["state"] for state in idle_states(processor) if state["disabled"]]
            for processor in online_processors()
        },
    }
    try:
        _SAVED_STATE.write_text(json.dumps(recorded, indent=2), encoding="utf-8")
    except OSError as error:
        print(f"  note: could not record the current state in {_SAVED_STATE} ({error}); --off will restore the defaults instead")


def apply_measuring_state() -> int:
    """Switch the machine into the state a measurement needs. Returns a process exit status."""
    if os.geteuid() != 0:
        print("This changes processor settings, so it has to run as root.")
        return 1

    save_current_state()

    changed_governor = 0
    for processor in online_processors():
        path = _CPU_BASE / f"cpu{processor}" / "cpufreq" / "scaling_governor"
        if read_value(path) != _MEASURING_GOVERNOR and write_value(path, _MEASURING_GOVERNOR):
            changed_governor += 1
    print(f"  governor    : set {_MEASURING_GOVERNOR} on {changed_governor} processor(s)")

    disabled = 0
    for processor in online_processors():
        for state in idle_states(processor):
            if state["latency_us"] > _IDLE_LATENCY_LIMIT_US and not state["disabled"]:
                if write_value(_CPU_BASE / f"cpu{processor}" / "cpuidle" / state["state"] / "disable", "1"):
                    disabled += 1
    print(f"  idle states : switched off {disabled} state(s) costing more than {_IDLE_LATENCY_LIMIT_US}us to leave")

    return report_state()


def restore_previous_state() -> int:
    """Put the machine back where it was. Returns a process exit status."""
    if os.geteuid() != 0:
        print("This changes processor settings, so it has to run as root.")
        return 1

    recorded: dict = {}
    if _SAVED_STATE.is_file():
        try:
            recorded = json.loads(_SAVED_STATE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            recorded = {}
    if recorded:
        print(f"  restoring what was recorded in {_SAVED_STATE}")
    else:
        print(f"  nothing recorded in {_SAVED_STATE}, so restoring the documented defaults:")
        print(f"  the {_DEFAULT_GOVERNOR} governor, and every idle state enabled")

    saved_governors = recorded.get("governors", {})
    saved_disabled = recorded.get("disabled_idle_states", {})

    for processor in online_processors():
        wanted = saved_governors.get(str(processor), _DEFAULT_GOVERNOR)
        path = _CPU_BASE / f"cpu{processor}" / "cpufreq" / "scaling_governor"
        if read_value(path) != wanted:
            write_value(path, wanted)

        should_be_off = set(saved_disabled.get(str(processor), []))
        for state in idle_states(processor):
            wanted_disabled = state["state"] in should_be_off
            if state["disabled"] != wanted_disabled:
                write_value(_CPU_BASE / f"cpu{processor}" / "cpuidle" / state["state"] / "disable",
                            "1" if wanted_disabled else "0")

    try:
        _SAVED_STATE.unlink()
    except OSError:
        pass

    return report_state(after_revert=True)


def report_state(after_revert: bool = False) -> int:
    """Print what state the machine is in.

    Returns 0 when measurements would mean something, which is what the measurement tooling
    checks. The one exception is after --off, where being unfit to measure is the point: see
    after_revert below.

    @param after_revert True when this is the report at the end of --off. Without it the same
                        words appear either way, and someone who has just asked for the machine
                        to be put back is told that a measurement would not mean much and that
                        they should fix it -- which reads as a failure when it is the thing they
                        asked for.
    """
    ready, problems = describe_state()

    using = governors()
    distinct = sorted(set(using.values()))
    print(f"  governor    : {', '.join(distinct) if distinct else 'unknown'} across {len(using)} processor(s)")

    for state in idle_states(online_processors()[0] if online_processors() else 0):
        mark = "off" if state["disabled"] else "ON "
        print(f"  idle state  : {mark} {state['name']:<10} {state['latency_us']}us to leave")

    print()
    if ready:
        if after_revert:
            # Everything was put back and the machine still reads as fit to measure on, which
            # means it was already in that state before --on ever ran. Worth saying, because
            # somebody left it that way on purpose and --off has not changed it.
            print("This machine is still in a state where a latency measurement means something,")
            print("which means it was already set up that way before any of this ran.")
            return 0
        print("This machine is in a state where a latency measurement means something.")
        return 0

    if after_revert:
        print("This machine is back to its ordinary settings. It is not fit to measure latency on,")
        print("and that is the point: the measuring settings cost power and fan noise continuously.")
        print()
        print("Before the next measuring session:  sudo python3 scripts/measurement_machine.py --on")
        return 0

    print("A latency measurement taken now would not mean much:")
    for problem in problems:
        print(f"  - {problem}")
    print()
    print("Fix it with:  sudo python3 scripts/measurement_machine.py --on")
    return 1


def parse_arguments() -> argparse.Namespace:
    """Command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--on", action="store_true", help="switch the machine into the state a measurement needs (root)")
    group.add_argument("--off", action="store_true", help="put the machine back where it was (root)")
    return parser.parse_args()


def main() -> int:
    """Report, apply, or revert.

    Exit status is 0 when the machine is fit to measure on, which is what the measurement tooling
    reads. After --off it is 0 when the machine was successfully put back, because there the
    operation succeeded and reporting a failure would stop any script that checks.
    """
    arguments = parse_arguments()
    if arguments.on:
        return apply_measuring_state()
    if arguments.off:
        return restore_previous_state()
    return report_state()


if __name__ == "__main__":
    sys.exit(main())
