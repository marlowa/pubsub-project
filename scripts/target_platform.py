#!/usr/bin/env python3
"""
target_platform.py -- which platform this machine is, and where its copy of the venue lives in the project tree.

The venue is built on two platforms: the ordinary development host, and Rocky Linux 8 or Red Hat Enterprise Linux 8,
where the compiler is gcc 8.5. A build for one platform cannot run on the other. A gcc 8.5 build links against an older
C library and carries a run path naming the build machine's third-party directory, so its binaries started on the
development host stop at once with exit status 127.

Both platforms can share one project tree. The Rocky 8 container mounts the developer's repository, so a build made in
the container writes into the same directories as a build made on the host. To keep the two apart, every directory
that one platform's build writes into the project tree is named with a suffix on Rocky or Red Hat 8: the staging
directory is installed-rocky8/ there and installed/ on the development host.

Everything that reads or writes such a directory must name it the same way, or one side looks in the directory the
other side deliberately avoided. This module is the one place that says how the name is formed. build.py, release.py,
devsetup.py and release_check.py use it to name what they write. devenv.py, deploy.py, cpu_audit.py and the operator's
tools use it to read the paths in an environment file.

A release ships this file beside devenv.py and deploy.py, which import it. It must therefore import nothing from this
project, and nothing outside the Python standard library.

Usage:
  ./target_platform.py
"""

from __future__ import annotations

import argparse
from pathlib import Path


# Every value platform_tag() can return. Needed to recognise another platform's artefact by name, which the ordinary
# development host cannot do from its own tag alone, because its tag is None.
known_platform_tags = ('rhel8', 'rocky8')


def platform_tag() -> str | None:
    """Short name for the target platform, or None when this is the ordinary development host.

    Used to keep build output for a different compiler apart. A gcc 8.5 Rocky or Red Hat 8 build is a different
    artefact from a gcc 13 build on the development host in the same way that a coverage or sanitizer build is: the
    same sources, but objects that cannot be mixed. If both shared a directory, every change of platform would at best
    be a full rebuild, and at worst a confusing mixture of stale objects and a stale CMake cache.
    """
    try:
        text = Path('/etc/os-release').read_text(encoding='utf-8')
    except OSError:
        return None
    fields = {line.split('=')[0]: line.split('=', 1)[1] for line in text.splitlines() if '=' in line}
    identifier = fields.get('ID', '').strip('"')
    version = fields.get('VERSION_ID', '').strip('"')
    if identifier in ('rhel', 'rocky', 'centos') and version.startswith('8'):
        return 'rhel8' if identifier == 'rhel' else 'rocky8'
    return None


def platform_suffix() -> str:
    """The suffix that tells this platform's build products apart from another platform's.

    Empty on the ordinary development host, so the names there are unchanged and only a Rocky or Red Hat 8 build is
    qualified. The staging directory follows the same rule: the host keeps installed/ and Rocky 8 gets installed-rocky8/.
    """
    tag = platform_tag()
    return f"-{tag}" if tag else ""


def environment_path(project_root: Path, configured: str) -> Path:
    """The absolute path that a path in an environment file's [paths] section names on this machine.

    An absolute path, such as /opt/pubsub in the production environment file, is the real location of the venue on a
    target host. A host runs only one platform, so the path is used as it is written.

    A relative path, such as installed or installed/log in the development environment file, names a directory inside
    the project tree, and is resolved from the project root. That tree may be shared between the development host and
    the Rocky 8 container, so the first directory of the path is given this platform's suffix: installed/log becomes
    installed-rocky8/log on Rocky 8 and stays installed/log on the development host. This is the directory build.py
    stages into on the same platform, so the scripts that deploy and run the venue find what the build wrote, and a
    deployment on one platform never writes over the other platform's binaries, configuration or logs.

    @param[in] project_root The directory that holds the environments/ directory.
    @param[in] configured   The path as the environment file writes it.
    @return The absolute path to use on this machine.
    """
    path = Path(configured)
    if path.is_absolute():
        return path.resolve()
    parts = path.parts
    if not parts:
        return project_root.resolve()
    qualified = Path(parts[0] + platform_suffix(), *parts[1:])
    return (project_root / qualified).resolve()


def main() -> None:
    """Print this machine's platform tag and suffix."""
    parser = argparse.ArgumentParser(
        description="Print the platform this machine is taken to be, and the suffix given to the directories that its builds and its "
                    "copy of the venue use inside the project tree.")
    parser.parse_args()
    tag = platform_tag()
    print(f"platform tag    : {tag if tag else '(none: the ordinary development host)'}")
    print(f"platform suffix : {platform_suffix()!r}")


if __name__ == "__main__":
    main()
