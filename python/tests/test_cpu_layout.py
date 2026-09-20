"""Tests for cpu_layout.py's allocation of cores to components.

cpu_layout.py lives in scripts/ rather than under python/, so it is loaded by path here, the
same way test_deploy_flatten.py loads deploy.py.

These cover the policy the allocator exists to enforce, which measurement settled on
2026-09-20 and which is not the obvious one.

A logical processor is not a core: where simultaneous multithreading is enabled each physical
core presents two, sharing execution units and level-one cache. The obvious conclusion is that
two hot-path threads must never share a core. That was measured and is wrong. A component's own
two threads -- its reactor thread and its application thread -- exchange messages constantly,
and sharing level-one cache makes that handoff nearly free. Separating them cost 10 per cent of
the mean inter-thread hop and five times the round trip's 99th percentile.

So sibling sharing is not automatically contention; it depends who the sibling is. The rule is
that a physical core is filled by one component or left alone.
"""

import importlib.util
import sys
from pathlib import Path

import pytest

_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(name="cpu_layout", scope="module")
def _cpu_layout():
    path = _REPOSITORY_ROOT / "scripts" / "cpu_layout.py"
    spec = importlib.util.spec_from_file_location("cpu_layout", path)
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves annotations through sys.modules, so the module has to be registered
    # before it is executed.
    sys.modules["cpu_layout"] = module
    spec.loader.exec_module(module)
    return module


def _multithreaded_topology(cpu_layout, physical_performance_cores=8, efficiency_cores=16):
    """A machine shaped like the development workstation.

    Performance cores come in pairs -- cpu0 and cpu1 are one core, cpu2 and cpu3 the next -- and
    efficiency cores present one processor each. That is the layout that made the original
    counting error invisible, so it is the one worth testing against.
    """
    cores = []
    for index in range(physical_performance_cores):
        first = index * 2
        for cpu_id in (first, first + 1):
            cores.append(cpu_layout.Core(cpu_id=cpu_id, numa_node_id=0,
                                         is_performance_core=True, physical_core_id=first))
    next_cpu = physical_performance_cores * 2
    for offset in range(efficiency_cores):
        cpu_id = next_cpu + offset
        cores.append(cpu_layout.Core(cpu_id=cpu_id, numa_node_id=0,
                                     is_performance_core=False, physical_core_id=cpu_id))
    return cores


def _resolve(cpu_layout, topology, component_count, threads_each=1, **kwargs):
    names = [f"component_{index}" for index in range(component_count)]
    return cpu_layout.resolve_layout(
        machine="test",
        components_on_machine=names,
        ranks={name: index + 1 for index, name in enumerate(names)},
        thread_counts={name: threads_each for name in names},
        topology=topology,
        **kwargs,
    )


def test_a_components_threads_share_one_physical_core(cpu_layout):
    """The finding that overturned the first guess.

    A component's reactor thread and application thread pass messages to each other constantly,
    and two hyperthreads of one core share level-one cache. Splitting them across cores makes
    every one of those messages cross a cache boundary.
    """
    topology = _multithreaded_topology(cpu_layout)
    layout = _resolve(cpu_layout, topology, component_count=4,
                      minimum_background_cores=4, threads_each=2)

    by_cpu = {core.cpu_id: core for core in topology}
    assert layout.component_cores, "nothing was admitted, so the test proves nothing"
    for name, cpus in layout.component_cores.items():
        physical = {by_cpu[cpu_id].physical_core_id for cpu_id in cpus}
        assert len(physical) == 1, (
            f"{name} was given cpus {cpus} spanning physical cores {sorted(physical)}; "
            f"its two threads should share one core"
        )


def test_no_physical_core_is_shared_between_components(cpu_layout):
    """The other half of the rule, and the half the original allocator broke.

    Two threads of different components cooperate over nothing. They only contend.
    """
    topology = _multithreaded_topology(cpu_layout)
    layout = _resolve(cpu_layout, topology, component_count=6,
                      minimum_background_cores=4, threads_each=2)

    by_cpu = {core.cpu_id: core for core in topology}
    owner_of_core = {}
    for name, cpus in layout.component_cores.items():
        for cpu_id in cpus:
            core_id = by_cpu[cpu_id].physical_core_id
            assert owner_of_core.setdefault(core_id, name) == name, (
                f"physical core {core_id} is shared by {owner_of_core[core_id]} and {name}"
            )


def test_a_core_given_to_one_component_is_not_lent_to_the_background_tier(cpu_layout):
    """A background thread on a hot-path core contends exactly as another hot-path thread would."""
    topology = _multithreaded_topology(cpu_layout)
    layout = _resolve(cpu_layout, topology, component_count=4,
                      minimum_background_cores=4, threads_each=2)

    by_cpu = {core.cpu_id: core for core in topology}
    hot_path_physical = {by_cpu[cpu_id].physical_core_id for cpu_id in layout.hot_path_cores}
    for cpu_id in layout.background_cores:
        assert by_cpu[cpu_id].physical_core_id not in hot_path_physical, (
            f"background processor {cpu_id} sits on a core already given to the hot path"
        )


def test_a_component_wanting_one_thread_still_takes_a_whole_core(cpu_layout):
    """The spare processor is left unused rather than lent out, and is reported as such."""
    topology = _multithreaded_topology(cpu_layout)
    layout = _resolve(cpu_layout, topology, component_count=3,
                      minimum_background_cores=4, threads_each=1)

    by_cpu = {core.cpu_id: core for core in topology}
    for name, cpus in layout.component_cores.items():
        assert len(cpus) == 1, f"{name} asked for one thread and got {cpus}"

    assert layout.idle_sibling_cores, "the unused processors were not reported"
    hot_path_physical = {by_cpu[cpu_id].physical_core_id for cpu_id in layout.hot_path_cores}
    for cpu_id in layout.idle_sibling_cores:
        assert by_cpu[cpu_id].physical_core_id in hot_path_physical
        assert cpu_id not in layout.background_cores


def test_reserving_cpu0_reserves_its_whole_physical_core(cpu_layout):
    """Reserving the processor alone reserves half a core.

    Its sibling shares the execution units the operating system is using, so handing that
    sibling to a hot-path thread defeats the reservation without reporting anything.
    """
    topology = _multithreaded_topology(cpu_layout)
    layout = _resolve(cpu_layout, topology, component_count=4,
                      minimum_background_cores=4, reserve_cpu0=True)

    claimed = set(layout.hot_path_cores) | set(layout.background_cores) | set(layout.idle_sibling_cores)
    assert 0 not in claimed
    assert 1 not in claimed, "cpu1 shares a physical core with the reserved cpu0"


def test_capacity_is_physical_cores_not_processors(cpu_layout):
    """Eight physical performance cores, one reserved, admit seven threads and not more.

    Before this, the allocator counted fifteen processors as fifteen cores and admitted
    fourteen threads onto seven cores, reporting success.
    """
    topology = _multithreaded_topology(cpu_layout, physical_performance_cores=8)
    layout = _resolve(cpu_layout, topology, component_count=10, minimum_background_cores=4)

    admitted = [group for group in layout.groups if group.admitted]
    assert len(layout.hot_path_cores) == 7
    assert len(admitted) == 7
    refused = [group for group in layout.groups if not group.admitted]
    assert refused, "nothing was refused, so the ceiling was not reached"
    assert "P-core" in refused[0].reason


def test_a_machine_without_multithreading_is_unaffected(cpu_layout):
    """Every processor is its own physical core there, so nothing is set aside."""
    topology = [
        cpu_layout.Core(cpu_id=cpu_id, numa_node_id=0, is_performance_core=cpu_id < 8,
                        physical_core_id=cpu_id)
        for cpu_id in range(24)
    ]
    layout = _resolve(cpu_layout, topology, component_count=5, minimum_background_cores=4)

    assert layout.idle_sibling_cores == []
    assert len(layout.hot_path_cores) == 5
    assert 0 not in layout.hot_path_cores, "cpu0 should still be reserved"
