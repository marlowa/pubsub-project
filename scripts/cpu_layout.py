#!/usr/bin/env python3
"""
cpu_layout.py -- resolve declared hot_path_rank values into concrete core ids.

The environment TOML declares *intent*: which processes run on each machine
([machines.*]) and the order in which they surrender entitlement to a dedicated
core (hot_path_rank on [components.*]).  It never declares core ids, because a
core id is machine-specific and must be re-derived for every host.  This module
turns the intent into an answer for one particular machine, by reading that
machine's real topology from sysfs.

deploy.py runs on the target host and takes no host argument, so the topology it
reads is the topology the components will run on.  Re-running deploy after a
hardware change recomputes the layout.

The admission rule, in full:

    claimable = online cores, minus cpu0 when reserve_cpu0
    groups    = components on this machine having hot_path_rank, by rank ascending

    for group in groups:
        demand = sum of hot_path_thread_count over the group
        if enough P-cores remain for demand, and
           claimable - |hot_path| - demand >= minimum_background_cores:
            admit the group, allocating P-cores first
        else:
            stop

    background = claimable - hot_path

Two properties of that loop carry the whole design:

  * A rank group is admitted **whole or not at all**, so components sharing a
    rank -- the two gateways -- can never be split across core types.  That is
    what makes the FIX-versus-binary comparison valid by construction rather
    than by arithmetic accident.
  * It **stops** rather than skips.  Once a group does not fit, no lower-ranked
    group is considered either; otherwise a small low-rank group could leapfrog
    a larger high-rank one.

See docs/design/cpu_pinning_anti_affinity.md.
"""

from __future__ import annotations

import socket
from dataclasses import dataclass, field
from pathlib import Path

_CPU_BASE = Path("/sys/devices/system/cpu")
_NODE_BASE = Path("/sys/devices/system/node")

# The maximum normalised capacity used by the Linux EAS scheduler.  A core
# reporting this is a P-core; anything lower is an E-core.
_EAS_MAXIMUM_CAPACITY = 1024

# ACPI CPPC highest_perf is not normalised, so classification is relative to the
# system-wide maximum.  A core below this percentage of that maximum is an
# E-core.  Mirrors detail::read_core_type() in CpuPinning.hpp.
_CPPC_P_CORE_THRESHOLD_PERCENT = 80


# -- Topology -----------------------------------------------------------------

@dataclass(frozen=True)
class Core:
    """One online CPU, with everything admission needs to place it."""

    cpu_id: int
    numa_node_id: int
    is_performance_core: bool

    @property
    def core_type_name(self) -> str:
        """Short label for reporting."""
        return "P-core" if self.is_performance_core else "E-core"


def parse_cpu_list(text: str) -> list[int]:
    """Parse a kernel CPU-list string such as '0-3,8-11' into a flat list."""
    cpus: list[int] = []
    for token in text.strip().split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            start, _, end = token.partition("-")
            cpus.extend(range(int(start), int(end) + 1))
        else:
            cpus.append(int(token))
    return cpus


def _read_maximum_cppc_performance() -> int:
    """System-wide maximum acpi_cppc/highest_perf, or 0 when unsupported.

    Used as the reference value for relative E-core classification.
    """
    maximum = 0
    if not _CPU_BASE.is_dir():
        return 0
    for entry in _CPU_BASE.iterdir():
        name = entry.name
        if not name.startswith("cpu") or not name[3:].isdigit():
            continue
        perf_path = entry / "acpi_cppc" / "highest_perf"
        if not perf_path.is_file():
            continue
        try:
            maximum = max(maximum, int(perf_path.read_text().strip()))
        except (OSError, ValueError):
            continue
    return maximum


def _is_performance_core(cpu_id: int, maximum_cppc_performance: int) -> bool:
    """Classify one core, mirroring detail::read_core_type() in CpuPinning.hpp.

    Two sources are tried in order: cpu_capacity (Linux EAS) then ACPI CPPC.
    When neither is available the core type is Unknown, which every caller
    treats as a P-core -- the safe fallback on uniform architectures, where the
    background reserve rather than the P-core ceiling is meant to bind.
    """
    capacity_path = _CPU_BASE / f"cpu{cpu_id}" / "cpu_capacity"
    if capacity_path.is_file():
        try:
            return int(capacity_path.read_text().strip()) >= _EAS_MAXIMUM_CAPACITY
        except (OSError, ValueError):
            pass

    if maximum_cppc_performance > 0:
        perf_path = _CPU_BASE / f"cpu{cpu_id}" / "acpi_cppc" / "highest_perf"
        if perf_path.is_file():
            try:
                perf = int(perf_path.read_text().strip())
                return perf * 100 >= maximum_cppc_performance * _CPPC_P_CORE_THRESHOLD_PERCENT
            except (OSError, ValueError):
                pass

    return True


def _read_numa_map() -> dict[int, int]:
    """Map cpu id to NUMA node id, empty when sysfs exposes no NUMA topology."""
    numa_map: dict[int, int] = {}
    if not _NODE_BASE.is_dir():
        return numa_map
    for node_dir in _NODE_BASE.iterdir():
        if not node_dir.name.startswith("node") or not node_dir.name[4:].isdigit():
            continue
        cpulist_path = node_dir / "cpulist"
        if not cpulist_path.is_file():
            continue
        node_id = int(node_dir.name[4:])
        try:
            for cpu_id in parse_cpu_list(cpulist_path.read_text()):
                numa_map[cpu_id] = node_id
        except (OSError, ValueError):
            continue
    return numa_map


def read_topology() -> list[Core]:
    """Read this machine's online cores, in the order cores should be handed out.

    The order is P-cores before E-cores and, within each tier, lower NUMA node
    first -- the same preference order get_available_cpu_ids() applies, so a
    caller taking the first N cores automatically prefers P-cores.
    """
    online_path = _CPU_BASE / "online"
    if not online_path.is_file():
        raise RuntimeError(f"cannot read CPU topology: {online_path} is absent")
    online = parse_cpu_list(online_path.read_text())

    maximum_cppc_performance = _read_maximum_cppc_performance()
    numa_map = _read_numa_map()

    cores = [
        Core(
            cpu_id=cpu_id,
            numa_node_id=numa_map.get(cpu_id, 0),
            is_performance_core=_is_performance_core(cpu_id, maximum_cppc_performance),
        )
        for cpu_id in online
    ]
    cores.sort(key=lambda core: (not core.is_performance_core, core.numa_node_id, core.cpu_id))
    return cores


# -- Resolution ---------------------------------------------------------------

class LayoutError(Exception):
    """A declared layout that cannot be resolved on this machine at all."""


@dataclass
class RankGroup:
    """One rank's worth of components, admitted or demoted as a unit."""

    rank: int
    components: list[str]
    demand: int
    admitted: bool = False
    reason: str = ""


@dataclass
class Layout:
    """The resolved answer for one machine."""

    machine: str
    claimable: list[Core]
    background_cores: list[int]
    minimum_background_cores: int
    reserve_cpu0: bool
    component_cores: dict[str, list[int]] = field(default_factory=dict)
    groups: list[RankGroup] = field(default_factory=list)
    unranked: list[str] = field(default_factory=list)
    quill_backend_cores: dict[str, int] = field(default_factory=dict)

    @property
    def hot_path_cores(self) -> list[int]:
        """Every core assigned to an admitted component, ascending."""
        return sorted(core for cores in self.component_cores.values() for core in cores)

    @property
    def demoted_groups(self) -> list[RankGroup]:
        """Rank groups that were not admitted, each carrying its reason."""
        return [group for group in self.groups if not group.admitted]


def resolve_layout(
    machine: str,
    components_on_machine: list[str],
    ranks: dict[str, int],
    thread_counts: dict[str, int],
    topology: list[Core],
    minimum_background_cores: int = 0,
    reserve_cpu0: bool = True,
    quill_backend_components: list[str] | None = None,
) -> Layout:
    """Resolve declared ranks into concrete core ids for one machine.

    :param machine: the machine key this layout is for, for reporting.
    :param components_on_machine: every process on the machine, ranked or not.
    :param ranks: component name to hot_path_rank, for those that have one.
        Absence means background -- forgetting to rank something places it where
        it almost certainly belonged.
    :param thread_counts: component name to hot-path thread demand.
    :param topology: this machine's cores, in hand-out order.
    :param minimum_background_cores: floor on the shared tier.  Its first job is
        correctness: the background pool must never be empty, because an empty
        affinity mask is EINVAL and every process has at least a Quill backend
        needing somewhere to run.
    :param quill_backend_components: components that have a Quill backend to
        place on a background core -- the C++ ones.  The JVMs have none.
    :raises LayoutError: on a configuration no machine could satisfy (case C).
    """
    claimable = [core for core in topology if not (reserve_cpu0 and core.cpu_id == 0)]

    # Case C: not a shortfall but a nonsensical configuration -- no group could
    # ever be admitted whatever the demand.  A hard error, not a demotion.
    if minimum_background_cores >= len(claimable):
        raise LayoutError(
            f"machine '{machine}': minimum_background_cores is "
            f"{minimum_background_cores} but only {len(claimable)} core(s) are claimable"
            f"{' after reserving cpu0' if reserve_cpu0 else ''} -- "
            f"no rank group could ever be admitted"
        )

    ranked = sorted(
        (name for name in components_on_machine if name in ranks),
        key=lambda name: (ranks[name], name),
    )
    groups: list[RankGroup] = []
    for name in ranked:
        rank = ranks[name]
        if groups and groups[-1].rank == rank:
            groups[-1].components.append(name)
            groups[-1].demand += thread_counts.get(name, 0)
        else:
            groups.append(RankGroup(rank=rank, components=[name],
                                    demand=thread_counts.get(name, 0)))

    performance_pool = [core for core in claimable if core.is_performance_core]
    layout = Layout(
        machine=machine,
        claimable=claimable,
        background_cores=[],
        minimum_background_cores=minimum_background_cores,
        reserve_cpu0=reserve_cpu0,
        unranked=sorted(name for name in components_on_machine if name not in ranks),
    )

    allocated = 0
    stopped_at_rank: int | None = None
    for group in groups:
        if stopped_at_rank is not None:
            group.reason = f"admission stopped at rank {stopped_at_rank}, which did not fit"
            continue

        remaining_performance = len(performance_pool) - allocated
        remaining_background = len(claimable) - allocated - group.demand

        if group.demand > remaining_performance:
            group.reason = (
                f"needs {group.demand} core(s), only {remaining_performance} P-core(s) remain"
            )
            stopped_at_rank = group.rank
        elif remaining_background < minimum_background_cores:
            group.reason = (
                f"needs {group.demand} core(s), which would leave {remaining_background} "
                f"background core(s), below the reserve of {minimum_background_cores}"
            )
            stopped_at_rank = group.rank
        else:
            group.admitted = True
            for name in group.components:
                count = thread_counts.get(name, 0)
                layout.component_cores[name] = [
                    core.cpu_id for core in performance_pool[allocated:allocated + count]
                ]
                allocated += count

    # Note that every group below the failure is reported as *stopped*, not as
    # individually rejected, because that is what happened: a later, smaller
    # group might well have fitted and is deliberately not given the chance.
    layout.groups = groups
    hot_path = set(layout.hot_path_cores)
    layout.background_cores = [core.cpu_id for core in claimable if core.cpu_id not in hot_path]

    # Quill backends are pinned to a specific background core rather than left to
    # drift under the scheduler, which means the background tier needs allocating
    # too -- pinning every backend to the same core would put thirteen of them in
    # contention on one CPU.  Round-robin over the background pool, assigned here
    # rather than guessed at runtime so the choice is visible in the layout file
    # and the affinity audit has something to check against.  Only components
    # with a Quill backend are given one; the JVMs have none.
    background_count = len(layout.background_cores)
    for index, name in enumerate(sorted(quill_backend_components or [])):
        layout.quill_backend_cores[name] = layout.background_cores[index % background_count]

    return layout


# -- Machine identity ---------------------------------------------------------

def select_machine(machines: dict, hostname: str | None = None) -> tuple[str, dict]:
    """Pick this host's entry from [machines.*].

    An exact match on the hostname or the fully-qualified name wins; 'localhost'
    is recognised as a fallback so dev.toml works unmodified on any workstation.
    """
    if not machines:
        raise LayoutError("the environment TOML declares no [machines.*] entries")

    raw = [hostname] if hostname is not None else [socket.gethostname(), socket.getfqdn()]
    # A short hostname should still match a fully-qualified machine key, and vice
    # versa.  dict.fromkeys dedupes while preserving order, which matters because
    # the first match wins and the list is shown to the operator on failure.
    candidates = list(dict.fromkeys(raw + [name.split(".", 1)[0] for name in raw]))

    for candidate in candidates:
        if candidate in machines:
            return candidate, machines[candidate]
    for candidate in candidates:
        for key in machines:
            if key.split(".", 1)[0] == candidate.split(".", 1)[0]:
                return key, machines[key]

    if "localhost" in machines:
        return "localhost", machines["localhost"]

    raise LayoutError(
        f"no [machines.*] entry matches this host (tried {', '.join(candidates)}); "
        f"declared machines are: {', '.join(sorted(machines))}"
    )


# -- Reporting ----------------------------------------------------------------

def format_layout(layout: Layout) -> str:
    """Render the computed layout for the deploy log.

    A demotion that appears in the output is diagnosable; one that appears only
    as unexplained latency is not.  So every group is named, admitted or not,
    and every demotion carries its reason.
    """
    lines = [
        f"  machine   : {layout.machine}",
        f"  claimable : {len(layout.claimable)} core(s)"
        f"{' (cpu0 reserved)' if layout.reserve_cpu0 else ''}"
        f", {sum(1 for c in layout.claimable if c.is_performance_core)} P-core(s)",
        f"  reserve   : {layout.minimum_background_cores} background core(s) minimum",
        "",
    ]

    for group in layout.groups:
        members = ", ".join(group.components)
        if group.admitted:
            placement = "; ".join(
                f"{name} -> {','.join(str(core) for core in layout.component_cores[name])}"
                for name in group.components
            )
            lines.append(f"  rank {group.rank}  ADMITTED  {members}")
            lines.append(f"            {placement}")
        else:
            lines.append(f"  rank {group.rank}  DEMOTED   {members}")
            lines.append(f"            {group.reason}")

    if layout.unranked:
        lines.append(f"  unranked  BACKGROUND  {', '.join(layout.unranked)}")

    lines.append("")
    lines.append(f"  hot-path  : {format_cpu_list(layout.hot_path_cores) or '(none)'}")
    lines.append(f"  background: {format_cpu_list(layout.background_cores) or '(none)'}")
    if layout.quill_backend_cores:
        placement = ", ".join(
            f"{name}->{core}" for name, core in sorted(layout.quill_backend_cores.items()))
        lines.append(f"  Quill backends on background cores: {placement}")

    # Case B: something was ranked and none of it was admitted.  A machine with
    # nothing ranked at all -- the admin host, say -- is not case B and must not
    # raise this, or the alarm is noise on every such host and gets ignored on
    # the one where it matters.
    if layout.groups and not layout.hot_path_cores:
        lines.append("")
        lines.append(
            "  *** NOTHING IS PINNED -- every ranked component was demoted and the ***\n"
            "  *** whole machine is background.                                    ***\n"
            "      This is correct on a small functional-test VM and alarming on a\n"
            "      production host, and the layout alone cannot tell those apart."
        )

    return "\n".join(lines)


def format_cpu_list(cores: list[int]) -> str:
    """Render a core list as a kernel cpu-list string: [1,2,3,7] -> '1-3,7'.

    Empty renders as the empty string, which is what the layout file needs; the
    human-readable report substitutes its own wording.
    """
    if not cores:
        return ""
    ordered = sorted(cores)
    ranges: list[str] = []
    start = previous = ordered[0]
    for cpu_id in ordered[1:]:
        if cpu_id == previous + 1:
            previous = cpu_id
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = cpu_id
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(ranges)


# -- The layout file ----------------------------------------------------------

def render_background_wrapper(layout: Layout) -> str:
    """Render the wrapper script that starts a process in the background tier.

    The mask has to be in force before the process runs, and a C++ component
    masking itself in main() cannot cover two cases: the window before main() is
    entered, and the JVM components, which have no main() of ours at all.
    fix_test_client is the one that matters most -- it is a JVM, it pins nothing,
    it drives both gateways and it saturates under load, so left unmasked it is
    free to be scheduled onto the gateways' own cores and contaminate the very
    measurement it exists to produce.

    taskset rather than a cgroup cpuset, because an affinity mask is not a
    ratchet: the Reactor can still promote a thread onto a hot-path core
    afterwards, which a cpuset would forbid.

    The script takes the command as "$@" and so needs to know nothing about how
    any component is launched -- that stays with the launcher.  It also gives
    perf or valgrind a single place to be interposed.
    """
    background = format_cpu_list(layout.background_cores)
    return f"""#!/bin/sh
# Generated by deploy.py -- do not edit.  Re-run deploy.py to recompute.
#
# Start a process in the background CPU tier of machine '{layout.machine}'.
# Threads the process creates inherit this mask, and the mask survives execve,
# which is what makes it work for the JVM components.
#
#   usage: background_tier <command> [args...]
#
# The Reactor promotes the few threads allocated dedicated cores after start-up.
# That still works from here: an affinity mask can be widened again, so this is
# a starting position rather than a cap.

exec taskset --cpu-list {background} "$@"
"""


def render_layout_file(layout: Layout) -> str:
    """Render the machine-wide layout file that components read at startup.

    One file per machine rather than the same facts scattered across fourteen
    component TOMLs: better for an operator, and it gives the affinity audit a
    single authority to check against.  Background assignments live here too,
    which is what lets the Quill backend be pinned explicitly to a background
    core without CpuRegistry growing a second pool.

    Core lists are kernel cpu-list strings ("1-3,7"), not TOML integer arrays.
    That is the format taskset -c takes, so the generated wrapper can pass one
    straight through, and the runtime parses it with the parse_cpu_list() that
    CpuPinning.hpp already has rather than needing a new accessor.  It also reads
    better for an operator than a seventeen-element array.
    """
    lines = [
        "# Generated by deploy.py -- do not edit.",
        "# Re-run deploy.py to recompute; editing this file will not change the",
        "# hardware it was computed for.",
        "",
        "[machine]",
        f'name = "{layout.machine}"',
        f"reserve_cpu0 = {'true' if layout.reserve_cpu0 else 'false'}",
        f"minimum_background_cores = {layout.minimum_background_cores}",
        f'claimable_cores = "{format_cpu_list([c.cpu_id for c in layout.claimable])}"',
        f'background_cores = "{format_cpu_list(layout.background_cores)}"',
        "",
    ]

    for group in layout.groups:
        for name in group.components:
            lines.append(f"[components.{name}]")
            lines.append(f"hot_path_rank = {group.rank}")
            lines.append(f"admitted = {'true' if group.admitted else 'false'}")
            if group.admitted:
                lines.append(f'hot_path_cores = "{format_cpu_list(layout.component_cores[name])}"')
            else:
                lines.append('hot_path_cores = ""')
                lines.append(f'demotion_reason = "{group.reason}"')
            if name in layout.quill_backend_cores:
                lines.append(f"quill_backend_core = {layout.quill_backend_cores[name]}")
            lines.append("")

    for name in layout.unranked:
        lines.append(f"[components.{name}]")
        lines.append("admitted = false")
        lines.append('hot_path_cores = ""')
        lines.append('demotion_reason = "not ranked -- background by default"')
        if name in layout.quill_backend_cores:
            lines.append(f"quill_backend_core = {layout.quill_backend_cores[name]}")
        lines.append("")

    return "\n".join(lines)
