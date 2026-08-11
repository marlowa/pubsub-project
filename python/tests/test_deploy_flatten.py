"""Tests for deploy.py's flattening of environment TOML into placeholder values.

deploy.py lives in scripts/ rather than under python/, so it is loaded by path here. It
has no package to import and no side effects at import time.

The array support these cover exists so a value several components must agree on -- the
order round-trip histogram's bucket bounds -- can be declared once in an environment file
and expanded into each gateway's config. Before it, flatten_toml silently skipped lists,
so the placeholder simply never resolved.
"""

import importlib.util
import sys
from pathlib import Path

import pytest

_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS_DIR = _REPOSITORY_ROOT / "scripts"
_DEPLOY_PATH = _SCRIPTS_DIR / "deploy.py"


def _load_deploy():
    # deploy.py imports cpu_layout, a sibling module in scripts/, so that has to be
    # importable before the module body runs.
    if str(_SCRIPTS_DIR) not in sys.path:
        sys.path.insert(0, str(_SCRIPTS_DIR))
    spec = importlib.util.spec_from_file_location("deploy_under_test", _DEPLOY_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(name="deploy", scope="module")
def deploy_fixture():
    return _load_deploy()


def test_integer_array_renders_as_toml_array(deploy):
    """The bucket-bounds case: bare integers, substituted into a component file verbatim."""
    flat = deploy.flatten_toml({"shared": {"metrics_order_round_trip_buckets": [10000, 25000, 50000]}})
    assert flat["shared_metrics_order_round_trip_buckets"] == "[10000, 25000, 50000]"


def test_float_array_keeps_its_decimals(deploy):
    flat = deploy.flatten_toml({"shared": {"bounds": [1.5, 2.25]}})
    assert flat["shared_bounds"] == "[1.5, 2.25]"


def test_string_array_elements_are_quoted(deploy):
    """The rendered text is substituted straight in, so it must be valid TOML by itself."""
    flat = deploy.flatten_toml({"shared": {"hosts": ["alpha", "beta"]}})
    assert flat["shared_hosts"] == '["alpha", "beta"]'


def test_boolean_array_uses_toml_literals(deploy):
    """Not Python's True/False, which TOML does not accept."""
    flat = deploy.flatten_toml({"shared": {"flags": [True, False]}})
    assert flat["shared_flags"] == "[true, false]"


def test_empty_array_renders_as_empty_brackets(deploy):
    flat = deploy.flatten_toml({"shared": {"nothing": []}})
    assert flat["shared_nothing"] == "[]"


def test_array_of_tables_is_still_skipped(deploy):
    """A component file needs its own [[section]] headers; no substitution can produce them."""
    flat = deploy.flatten_toml({"credential": [{"comp_id": "ALPHA"}]})
    assert "credential" not in flat


def test_scalars_and_nesting_are_unaffected(deploy):
    flat = deploy.flatten_toml(
        {"shared": {"metrics_enabled": False, "metrics_application": "pubsub"}, "witness": {"metrics_listen_port": 9200}}
    )
    assert flat["shared_metrics_enabled"] == "false"
    assert flat["shared_metrics_application"] == "pubsub"
    assert flat["witness_metrics_listen_port"] == "9200"


def test_every_environment_file_defines_the_shared_bucket_placeholder(deploy):
    """Each environment must resolve ${shared_metrics_order_round_trip_buckets}.

    An undefined placeholder makes deploy.py exit, so a missing entry here would stop a
    deployment rather than merely losing a metric.
    """
    environments_dir = _REPOSITORY_ROOT / "environments"
    environment_files = sorted(environments_dir.glob("*.toml"))
    assert environment_files, "no environment files found"

    for environment_file in environment_files:
        flat = deploy.flatten_toml(deploy.load_env(environment_file))
        rendered = flat.get("shared_metrics_order_round_trip_buckets")
        assert rendered is not None, f"{environment_file.name} does not define it"
        assert rendered.startswith("[") and rendered.endswith("]"), f"{environment_file.name}: {rendered}"


def test_the_bucket_bounds_agree_across_environments(deploy):
    """Not required for correctness, but a difference between environments is worth knowing.

    A histogram is only comparable against another with the same bounds, so dev and preprod
    diverging would mean a latency figure measured in one could not be checked in the other.
    """
    environments_dir = _REPOSITORY_ROOT / "environments"
    bounds_by_environment = {
        environment_file.name: deploy.flatten_toml(deploy.load_env(environment_file))["shared_metrics_order_round_trip_buckets"]
        for environment_file in sorted(environments_dir.glob("*.toml"))
    }
    assert len(set(bounds_by_environment.values())) == 1, \
        f"bucket bounds differ between environments: {bounds_by_environment}"


def test_overriding_the_port_moves_every_consumer_together(deploy):
    """--db-port must reach the JDBC URL as well as the [db] section.

    The RHEL8 host at work runs its cluster on a port the environment file does not name, and
    a deploy there failed exporting credentials. Overriding only the psql calls would fix that
    one symptom and leave the Java admin service deployed against a port with nothing on it --
    a clean deploy that fails later, which is worse than the failure it replaced.
    """
    environment = deploy.load_env(_REPOSITORY_ROOT / "environments" / "dev.toml")
    deploy.override_db_port(environment, 6543)

    assert environment["db"]["port"] == 6543
    assert environment["admin_service"]["db_url"] == "jdbc:postgresql://localhost:6543/pubsub"

    # The component templates expand these, so an override that stopped here would be silently
    # undone by the next deploy.
    flat = deploy.flatten_toml(environment)
    assert flat["db_port"] == "6543"
    assert "6543" in flat["admin_service_db_url"]


def test_the_override_leaves_the_two_sources_agreeing(deploy):
    environment = deploy.load_env(_REPOSITORY_ROOT / "environments" / "dev.toml")
    deploy.override_db_port(environment, 6543)
    # Raises SystemExit if the [db] section and the JDBC URL disagree.
    deploy.check_admin_service_db_url(environment)


def test_a_disagreeing_url_is_refused(deploy):
    """The check exists because a comment was the only thing holding the two together."""
    environment = deploy.load_env(_REPOSITORY_ROOT / "environments" / "dev.toml")
    environment["db"]["port"] = 6543
    with pytest.raises(SystemExit):
        deploy.check_admin_service_db_url(environment)


def test_an_environment_without_an_admin_service_is_left_alone(deploy):
    """Overriding must not invent a section. Not every environment deploys the Java service."""
    environment = {"db": {"port": 5432}}
    deploy.override_db_port(environment, 6543)
    assert environment["db"]["port"] == 6543
    assert "admin_service" not in environment
