# Building, deploying and running

Everything needed to take a fresh clone to a running system on one machine: build it, package it,
deploy it, and start it.

For the build itself in depth — the flavours, why cleaning between them is unnecessary, and where
instrumented artefacts go — see [building](building.md). This page is the path through, not the
detail of each step.

> **One thing none of this can do for you.** The filesystem holding the sequencer's log must be
> mounted `lazytime`, or the venue stalls for hundreds of milliseconds at a time under load. See
> [filesystem requirements](../operations/filesystem_requirements.md).

## Developer Quick-start (`devsetup.sh`)

The fastest path from source to a running sandbox is the convenience wrapper, which runs all three steps — build, release, deploy — in sequence:

```bash
./scripts/devsetup.sh                        # first time (creates DB)
./scripts/devsetup.sh --skip-create-db       # subsequent runs (DB already exists)
```

Once setup completes, start the stack:

```bash
python3 scripts/devenv.py start
```

`devsetup.sh` sets the required environment variables (third-party library paths and versions) and forwards all arguments to `devsetup.py`. Any flag accepted by the build or deploy steps can be passed through — see `./scripts/devsetup.sh --help`.

## Code formatting (clang-format)

C++ is formatted with `clang-format` per the root `.clang-format`. A pinned version is installed with the Python dev extras so every machine (dev and RHEL 8) formats identically, regardless of the OS-provided clang-format:

```bash
pip install -e python[dev]          # provides the pinned clang-format
scripts/install-git-hooks.sh        # enable the pre-commit hook (once per clone)
```

The pre-commit hook (`.githooks/pre-commit`) checks **only the lines each commit touches** via `git clang-format`, so the existing tree does not need to be fully reformatted first. If a staged change is not clean, the commit is blocked with the exact diff and this fix:

```bash
git clang-format --staged && git add -u
```

## Building (`build.py` / `build.sh`)

```bash
./scripts/build.sh
```

Builds both the C++ components and the Java admin service, runs all tests, and stages the result into `build/installed/`. This staging directory is what `release.py` reads from — it is not the runtime location.

Unit tests and integration tests run automatically. The build script reports signal-based failures (SIGABRT, SIGSEGV, etc.) by name.

Common options:

| Flag | Effect |
|---|---|
| `--no-java` | Skip the Java admin service build |
| `--no-cpp` | Skip the C++ build; build Java only |
| `--clean` | Clean before building (C++: deletes `build/`; Java: runs `mvn clean`) |
| `--no-tests` | Skip all tests (C++ unit/integration tests and Maven Surefire) |
| `--valgrind` | C++ build with Valgrind-compatible options (disables lock-free optimisations) |
| `--doxygen` | Generate Doxygen documentation after the C++ build |
| `-j N` | C++ build parallelism (default: all CPUs) |

`build.sh` is a thin wrapper that sets the platform-specific environment variables required by CMake and then calls `build.py`.

## Building on RHEL 8 with Docker

The `Dockerfile` at the project root provides a Rocky Linux 8 build environment that matches the RHEL 8 production target. Use it to verify RHEL 8 compatibility without access to a physical RHEL 8 machine.

### Step 1 — Install Docker (once, on your Mint machine)

```bash
sudo apt install docker.io
sudo usermod -aG docker $USER
```

Log out and back in after the `usermod` step so the group membership takes effect. Verify with:

```bash
docker run --rm hello-world
```

### Step 2 — Build the image (once, or when the Dockerfile changes)

From the project root:

```bash
docker build -t pubsub-rhel8 .
```

This downloads Rocky Linux 8, installs the compiler toolchain and PostgreSQL, and saves the result as a local image called `pubsub-rhel8`. It takes a few minutes the first time; subsequent builds are fast because Docker caches layers.

### Step 3 — Create the database volume (once, ever)

Docker containers are thrown away when they exit. A **named volume** gives the PostgreSQL data directory a permanent home on your host so the database survives across container runs:

```bash
docker volume create pubsub-pgdata
```

### Step 4 — Get a Rocky Linux shell

```bash
docker run -it --rm \
    -v "$(pwd)":/workspace \
    -v /path/to/thirdparty:/development/3rdparty \
    -v pubsub-pgdata:/var/lib/pgsql/data \
    pubsub-rhel8
```

You are now at a bash prompt inside Rocky Linux 8. The flags mean:

| Flag | Effect |
|---|---|
| `-it` | Interactive terminal — required for a usable shell |
| `--rm` | Delete the container automatically when you type `exit` |
| `-v "$(pwd)":/workspace` | Mounts the project root into the container at `/workspace`; edits are shared instantly in both directions |
| `-v /path/to/thirdparty:/development/3rdparty` | Pre-built third-party libraries (fmt, quill, etc.) built for Rocky 8. This is the path the real RHEL8 build hosts use, and it must stay outside `/workspace`: CMake leaves directories inside the project tree out of the install RPATH, so a tree mounted under the project links but is not found at run time |
| `-v pubsub-pgdata:/var/lib/pgsql/data` | Persistent PostgreSQL data directory |

The container entrypoint initialises the PostgreSQL cluster (first run only) and starts the server before dropping you into the shell.

### Step 5 — Set up the database (first time inside the container)

```bash
./scripts/build-release-deploy.sh --no-java --no-pylint --sudo-postgres
```

`--sudo-postgres` causes `create_db.py` to run `psql` as the `postgres` Unix user, which is required for peer authentication. `--no-java` is needed because the image does not include Java or Maven.

### Step 6 — Subsequent runs

Start a new shell the same way as Step 4. The database already exists on the volume, so pass `--skip-db`:

```bash
./scripts/build-release-deploy.sh --no-java --no-pylint --skip-db
```

### Build and test only (no deploy, no database needed)

If you only want to compile and run the C++ tests, omit the database volume entirely:

```bash
docker run -it --rm \
    -v "$(pwd)":/workspace \
    -v /path/to/thirdparty:/development/3rdparty \
    pubsub-rhel8
```

Then inside the container:

```bash
./scripts/build.sh --no-java --no-pylint
```

### Notes

- **Pylint:** `--no-pylint` is recommended because the pylint version on Rocky 8 may differ from the development machine and produce false positives.
- **Ninja vs Make:** `build.sh` respects the `CMAKE_GENERATOR` environment variable. Add `-e CMAKE_GENERATOR=Ninja` to the `docker run` command if ninja is installed in the container.
- **Java builds:** `admin-service` and `fix-test-client` cannot be built inside the container as supplied. To add Java support, extend the Dockerfile with `java-11-openjdk-devel` and `maven` packages.

## Packaging (`release.py`)

Assembles a versioned deployment artefact from the build staging area:

```bash
python3 scripts/release.py
```

Reads the version from `project(... VERSION x.y.z ...)` in `CMakeLists.txt` and the git short hash from `git rev-parse`. Reads binaries and the admin-service JAR from `build/installed/`. Outputs `build/release/pubsub-<version>-<hash>.tar.gz` containing `bin/`, `lib/`, `etc/` (config templates with unexpanded `${placeholder}` values), `db/`, `environments/`, `devenv.py`, `deploy.py`, and a `release.json` manifest.

A build for a platform other than the development host appends its tag, giving
`pubsub-<version>-<hash>-<mode>-rocky8.tar.gz`. A release tree is not portable between the two —
a gcc-8.5 build links against an older glibc and names its own third-party tree in the RPATH —
and the release directory is shared with the Rocky container, so both artefacts land side by
side and the name is the only thing telling them apart.

Options: `--install-dir` (staging dir, default: `build/installed`), `--env`, `--version`, `--output-dir`, `--no-git-hash`.

## Deployment (`deploy.py`)

Unpacks a release artefact and prepares it for launch:

```bash
python3 scripts/deploy.py --env environments/prod.toml \
                  --artefact pubsub-<version>-<hash>.tar.gz \
                  --install-dir /opt/pubsub \
                  --skip-certs
```

Steps performed in order:

1. **Unpack** the artefact into the install directory, stripping its top-level directory.
2. **Expand config templates** — substitutes `${placeholder}` values in all `etc/**/*.toml` files. Placeholder names are derived mechanically from the environment TOML by flattening every section and key into a single string: `[section] key` → `${section_key}`. For example, `[arbiter_primary] peer_host` in the env TOML becomes `${arbiter_primary_peer_host}` in the component template. A small number of placeholders are injected programmatically by `deploy.py` itself rather than read from the env TOML (currently `${paths_install_dir}`, `${shared_reactor_cpu_registry_shm_path}`, and `${shared_reactor_cpu_registry_lock_file}`). An undefined placeholder causes a hard exit naming the file and the missing key — there are no silent failures.

   **Tracing a placeholder:** if you see `${foo_bar_baz}` in an application template and cannot find its value, either (a) open the env TOML and look for a `[foo]` section with key `bar_baz`, or (b) search `deploy.py` for `namespace["foo_bar_baz"]`.
3. **Generate TLS certificates** — self-signed via `openssl req -x509` for each `[tls.*]` section. Pass `--skip-certs` when placing CA-signed certificates for production.
4. **Create the database** — delegates to `db/create_db.py`.
5. **Export SCRAM credentials** — delegates to `db/export_credentials.py`.

The install directory defaults to `paths.install_dir` from the env TOML (`installed/` for dev, `/opt/pubsub` for prod).

Options: `--skip-certs`, `--force-certs`, `--skip-db`, `--skip-create-db`, `--drop-db`, `--sudo-postgres`, `--liquibase-contexts`.

## Developer Sandbox (`devenv.py`)

`devenv.py` starts, stops, and monitors the full component stack on a developer machine. It reads component definitions and paths from an environment TOML (default: `environments/dev.toml`).

**Prerequisite:** run `devsetup.sh` (or the three steps manually) before the first start.

**Starting everything:**

```bash
python3 scripts/devenv.py start
```

Components are started in the order defined in `[startup_order]` in the env TOML, with a 1-second delay between each. Logs go to `installed/log/<name>.log` (application log) and `installed/log/<name>.stdout` (stdout/stderr). PID files go to `/var/tmp/pubsub/run/<name>.pid`.

**Checking status:**

```bash
python3 scripts/devenv.py status
```

**Stopping everything:**

```bash
python3 scripts/devenv.py stop
```

Components are stopped in reverse startup order. Stale PID files are cleaned up automatically.

**Restarting a single component** (useful during development iteration):

```bash
python3 scripts/devenv.py restart sequencer
python3 scripts/devenv.py restart               # restarts everything
```

**Skipping HA components** (run without arbiters, witness, and secondary instances):

```bash
python3 scripts/devenv.py --no-ha start
```

**Using a different environment:**

```bash
python3 scripts/devenv.py --env environments/test-1.toml start
```

**Options summary:**

| Flag | Default | Effect |
|---|---|---|
| `--env PATH` | `environments/dev.toml` | Environment TOML to use |
| `--no-ha` | off | Skip components with `ha_only = true` |
| `--delay SECONDS` | `1.0` | Pause between component starts |

### Optional: netfilter on loopback

Every component runs on one machine in a developer sandbox, so all of the venue's traffic
crosses `lo`. Netfilter hooks fire on every packet regardless of interface, loopback included,
and about 13% of the CPU in both the gateway and the matching engine profiles is `nftables`
and `conntrack` as a result -- `nft_do_chain`, `nft_counter_eval` and `nft_immediate_eval` sit
near the top of both.

None of it is the venue's doing, and none of it exists in a real deployment, where the
instances sit on separate machines and the traffic goes over a network card. It is worth
removing only when the profile itself is what you are looking at, so that two runs compare:

```bash
sudo nft flush ruleset          # removes the overhead for this boot
```

**Do not do this on a deployed host.** The ruleset is there deliberately, and flushing it for
the benefit of a benchmark is a change to that host's firewall. A run made without flushing is
a valid run; it simply carries a known overhead that production does not.

See [Trading-day load](../operations/trading_day_load.md) for the measurements.

---

Back to [orientation](../orientation/README.md), or the [documentation contents](../README.md).
