"""Tests for the build and developer-setup command options."""

from pathlib import Path
import subprocess
import sys

SCRIPTS_DIRECTORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import build  # noqa: E402
import devsetup  # noqa: E402


def test_build_help_lists_skip_tla():
    result = subprocess.run(
        [sys.executable, str(SCRIPTS_DIRECTORY / "build.py"), "--help"],
        check=True,
        capture_output=True,
        text=True,
    )

    assert "--skip-tla" in result.stdout


def test_cmake_enables_tla_pages_by_default(monkeypatch):
    commands = []
    monkeypatch.setattr(build, "run_command", lambda command, **kwargs: commands.append(command))

    build.configure_cmake(Path("build"), Path("source"))

    assert "-DENABLE_TLA_PAGES=ON" in commands[0]


def test_cmake_disables_tla_pages_when_requested(monkeypatch):
    commands = []
    monkeypatch.setattr(build, "run_command", lambda command, **kwargs: commands.append(command))

    build.configure_cmake(Path("build"), Path("source"), enable_tla_pages=False)

    assert "-DENABLE_TLA_PAGES=OFF" in commands[0]


def test_devsetup_forwards_skip_tla_to_build(monkeypatch):
    commands = []
    monkeypatch.setattr(sys, "argv", ["devsetup.py", "--skip-tla"])
    monkeypatch.setattr(devsetup, "_run", lambda command, step: commands.append(command))
    monkeypatch.setattr(devsetup, "_prune_old_artefacts", lambda keep: None)
    monkeypatch.setattr(devsetup, "_find_tarball", lambda: Path("release.tar.gz"))
    monkeypatch.setattr(devsetup, "_staging_dir", lambda: Path("installed"))

    devsetup.main()

    assert commands[0][-1] == "--skip-tla"


def test_a_named_compiler_has_its_own_build_and_staging_directories():
    options = build.argparse.Namespace(coverage=False, asan=False, tsan=False, valgrind=False, compiler="clang-14")

    assert build.build_directory_name(options).startswith("build-clang-14")
    assert build.install_directory_name(options).startswith("installed-clang-14")


def test_cmake_is_given_the_named_compiler_and_its_cxx_partner(monkeypatch):
    commands = []
    monkeypatch.setattr(build, "run_command", lambda command, **kwargs: commands.append(command))

    build.configure_cmake(Path("build"), Path("source"), compiler="clang-14")

    assert "-DCMAKE_C_COMPILER=clang-14" in commands[0]
    assert "-DCMAKE_CXX_COMPILER=clang++-14" in commands[0]


def test_cmake_is_given_no_compiler_unless_one_is_named(monkeypatch):
    commands = []
    monkeypatch.setattr(build, "run_command", lambda command, **kwargs: commands.append(command))

    build.configure_cmake(Path("build"), Path("source"))

    assert not any(argument.startswith("-DCMAKE_CXX_COMPILER") for argument in commands[0])


def test_keep_going_is_spelt_for_the_generator_the_build_directory_uses(tmp_path):
    (tmp_path / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Ninja\n", encoding="utf-8")
    assert build.keep_going_arguments(tmp_path) == ["--", "-k", "0"]

    (tmp_path / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n", encoding="utf-8")
    assert build.keep_going_arguments(tmp_path) == ["--", "-k"]
