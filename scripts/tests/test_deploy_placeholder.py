"""Tests for deploy.py's refusal of a placeholder environment file."""

from pathlib import Path
import sys

import pytest

SCRIPTS_DIRECTORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import deploy  # noqa: E402

ENVIRONMENTS = SCRIPTS_DIRECTORY.parent / "environments"


@pytest.mark.parametrize("name", ["prod", "preprod", "test-1"])
def test_each_placeholder_environment_is_refused_unless_asked_for(name):
    path = ENVIRONMENTS / f"{name}.toml"
    environment = deploy.load_env(path)
    with pytest.raises(SystemExit):
        deploy.refuse_a_placeholder_environment(environment, path, allowed=False)
    deploy.refuse_a_placeholder_environment(environment, path, allowed=True)


def test_the_development_environment_is_not_a_placeholder():
    path = ENVIRONMENTS / "dev.toml"
    environment = deploy.load_env(path)
    deploy.refuse_a_placeholder_environment(environment, path, allowed=False)
