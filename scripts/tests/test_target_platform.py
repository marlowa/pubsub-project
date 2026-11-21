"""Tests for target_platform.py: how a path in an environment file's [paths] section becomes a directory on this machine."""

from pathlib import Path
import subprocess
import sys

SCRIPTS_DIRECTORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import build  # noqa: E402
import devenv  # noqa: E402
import target_platform  # noqa: E402


def pretend_platform(monkeypatch, tag):
    """Make target_platform report the given tag, or the development host when tag is None."""
    monkeypatch.setattr(target_platform, "platform_tag", lambda: tag)


def test_a_relative_path_is_unchanged_on_the_development_host(monkeypatch, tmp_path):
    pretend_platform(monkeypatch, None)
    assert target_platform.environment_path(tmp_path, "installed") == tmp_path / "installed"
    assert target_platform.environment_path(tmp_path, "installed/log") == tmp_path / "installed" / "log"


def test_a_relative_path_carries_the_suffix_on_its_first_directory_on_rocky_8(monkeypatch, tmp_path):
    pretend_platform(monkeypatch, "rocky8")
    assert target_platform.environment_path(tmp_path, "installed") == tmp_path / "installed-rocky8"
    assert target_platform.environment_path(tmp_path, "installed/log") == tmp_path / "installed-rocky8" / "log"
    assert target_platform.environment_path(tmp_path, "installed/archive") == tmp_path / "installed-rocky8" / "archive"


def test_an_absolute_path_is_unchanged_on_every_platform(monkeypatch, tmp_path):
    for tag in (None, "rocky8", "rhel8"):
        pretend_platform(monkeypatch, tag)
        assert target_platform.environment_path(tmp_path, "/opt/pubsub") == Path("/opt/pubsub")
        assert target_platform.environment_path(tmp_path, "/opt/pubsub/log") == Path("/opt/pubsub/log")


def test_devenv_finds_the_directory_build_py_stages_into(monkeypatch):
    """The run side and the build side must name the same tree, on both platforms: build.py stages into
    installed<suffix>, and devenv.py reads the development environment file, whose install directory is "installed"."""
    env = devenv.load_env(devenv.PROJECT_ROOT / "environments" / "dev.toml")
    for tag in (None, "rocky8", "rhel8"):
        pretend_platform(monkeypatch, tag)
        install_dir, log_dir, run_dir = devenv.resolve_paths(env)
        staging_dir = (devenv.PROJECT_ROOT / ("installed" + target_platform.platform_suffix())).resolve()
        assert install_dir == staging_dir
        assert log_dir.parent == staging_dir
        assert run_dir.parent == staging_dir
        assert devenv.archive_dir(env).parent == staging_dir


def test_build_uses_the_same_platform_functions():
    assert build.platform_tag is target_platform.platform_tag
    assert build.platform_suffix is target_platform.platform_suffix


def test_help_describes_and_does_nothing_else():
    result = subprocess.run([sys.executable, str(SCRIPTS_DIRECTORY / "target_platform.py"), "--help"],
                            check=True, capture_output=True, text=True)
    assert "platform" in result.stdout
    assert "platform tag" not in result.stdout
