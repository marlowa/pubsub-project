# pubsub_itc_fw

A low-latency, multi-threaded, event-driven application framework for C++17, built around the **reactor pattern**. It provides inter-thread communication, inter-process communication, pub/sub messaging, timers, high availability, and a binary serialisation DSL — all designed for environments where heap allocation on the hot path is not acceptable.

## Features

- **Inter-thread communication (ITC)** via lock-free MPSC queues
- **Inter-process communication (IPC)** via unicast TCP with zero-copy PDU paths
- **Pub/sub messaging** via unicast fanout
- **Timers** via `timerfd` and `epoll`
- **High availability** via primary/secondary instance pairs with external arbiter pool and automatic leader election
- **Binary serialisation DSL** — a Python code generator producing C++17 encode/decode headers; sub-100ns round-trip on typical messages

## Design Principles

- CPU-pinned threads with lock-free fast paths throughout
- No heap allocation on any hot path — pool allocators, bump allocators, and slab allocators used exclusively
- Zero-copy on all inbound and outbound PDU paths
- Deterministic shutdown
- Message ordering preserved

## High Availability

The framework provides a built-in leader-follower protocol for deploying resilient application pairs. Two application instances are deployed — primary and secondary — and leader election is deterministic: the node with the lowest configured `instance_id` wins.

A separate pool of up to three dedicated arbiter processes (arbiter_primary, arbiter_secondary, witness) provides external arbitration to prevent split-brain when both nodes are undecided. Once elected, the peer-to-peer connection between the two application nodes is maintained with heartbeats. If the leader fails, the follower promotes itself and increments the epoch, ensuring that any restarting node can immediately recognise it is stale and rejoin as follower without requiring further arbitration.

The protocol is intentionally simple — there is no need for a full consensus algorithm such as Raft or Paxos given the fixed two-node-plus-arbiter topology.

## Serialisation DSL

Messages are defined in a lightweight DSL and compiled to C++17 headers by a Python code generator:

```
message StatusQuery (id=100, version=1)
    i64 instance_id
    i32 epoch
end
```

Supported field types include `i8`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`, `string`, `array<T>[N]`, `list<T>`, `optional T`, and named enum and message references. The wire format is little-endian binary. On little-endian hosts, `list<primitive>` decode is zero-copy.

## Requirements

| Item | Detail |
|---|---|
| Language | C++17 |
| Target compiler | gcc-8.5 / RHEL 8 |
| Build system | CMake + `build.py` |
| Logging | Quill v11.x |
| Test framework | GoogleTest (C++), pytest (DSL tests) |

## Developer Quick-start (`devsetup.sh`)

The fastest path from source to a running sandbox is the convenience wrapper, which runs all three steps — build, release, deploy — in sequence:

```bash
./devsetup.sh                        # first time (creates DB)
./devsetup.sh --skip-create-db       # subsequent runs (DB already exists)
```

Once setup completes, start the stack:

```bash
python3 devenv.py start
```

`devsetup.sh` sets the required environment variables (third-party library paths and versions) and forwards all arguments to `devsetup.py`. Any flag accepted by the build or deploy steps can be passed through — see `./devsetup.sh --help`.

## Building (`build.py` / `build.sh`)

```bash
./build.sh
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
    -v /path/to/thirdparty:/workspace/thirdparty \
    -v pubsub-pgdata:/var/lib/pgsql/data \
    pubsub-rhel8
```

You are now at a bash prompt inside Rocky Linux 8. The flags mean:

| Flag | Effect |
|---|---|
| `-it` | Interactive terminal — required for a usable shell |
| `--rm` | Delete the container automatically when you type `exit` |
| `-v "$(pwd)":/workspace` | Mounts the project root into the container at `/workspace`; edits are shared instantly in both directions |
| `-v /path/to/thirdparty:/workspace/thirdparty` | Pre-built third-party libraries (fmt, quill, etc.) built for Rocky 8 |
| `-v pubsub-pgdata:/var/lib/pgsql/data` | Persistent PostgreSQL data directory |

The container entrypoint initialises the PostgreSQL cluster (first run only) and starts the server before dropping you into the shell.

### Step 5 — Set up the database (first time inside the container)

```bash
./build-release-deploy.sh --no-java --no-pylint --sudo-postgres
```

`--sudo-postgres` causes `create_db.py` to run `psql` as the `postgres` Unix user, which is required for peer authentication. `--no-java` is needed because the image does not include Java or Maven.

### Step 6 — Subsequent runs

Start a new shell the same way as Step 4. The database already exists on the volume, so pass `--skip-db`:

```bash
./build-release-deploy.sh --no-java --no-pylint --skip-db
```

### Build and test only (no deploy, no database needed)

If you only want to compile and run the C++ tests, omit the database volume entirely:

```bash
docker run -it --rm \
    -v "$(pwd)":/workspace \
    -v /path/to/thirdparty:/workspace/thirdparty \
    pubsub-rhel8
```

Then inside the container:

```bash
./build.sh --no-java --no-pylint
```

### Notes

- **Pylint:** `--no-pylint` is recommended because the pylint version on Rocky 8 may differ from the development machine and produce false positives.
- **Ninja vs Make:** `build.sh` respects the `CMAKE_GENERATOR` environment variable. Add `-e CMAKE_GENERATOR=Ninja` to the `docker run` command if ninja is installed in the container.
- **Java builds:** `admin-service` and `fix-test-client` cannot be built inside the container as supplied. To add Java support, extend the Dockerfile with `java-11-openjdk-devel` and `maven` packages.

## Packaging (`release.py`)

Assembles a versioned deployment artefact from the build staging area:

```bash
python3 release.py
```

Reads the version from `project(... VERSION x.y.z ...)` in `CMakeLists.txt` and the git short hash from `git rev-parse`. Reads binaries and the admin-service JAR from `build/installed/`. Outputs `build/release/pubsub-<version>-<hash>.tar.gz` containing `bin/`, `lib/`, `etc/` (config templates with unexpanded `${placeholder}` values), `db/`, `environments/`, `devenv.py`, `deploy.py`, and a `release.json` manifest.

Options: `--install-dir` (staging dir, default: `build/installed`), `--env`, `--version`, `--output-dir`, `--no-git-hash`.

## Deployment (`deploy.py`)

Unpacks a release artefact and prepares it for launch:

```bash
python3 deploy.py --env environments/prod.toml \
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
python3 devenv.py start
```

Components are started in the order defined in `[startup_order]` in the env TOML, with a 1-second delay between each. Logs go to `installed/log/<name>.log` (application log) and `installed/log/<name>.stdout` (stdout/stderr). PID files go to `/var/tmp/pubsub/run/<name>.pid`.

**Checking status:**

```bash
python3 devenv.py status
```

**Stopping everything:**

```bash
python3 devenv.py stop
```

Components are stopped in reverse startup order. Stale PID files are cleaned up automatically.

**Restarting a single component** (useful during development iteration):

```bash
python3 devenv.py restart sequencer
python3 devenv.py restart               # restarts everything
```

**Skipping HA components** (run without arbiters, witness, and secondary instances):

```bash
python3 devenv.py --no-ha start
```

**Using a different environment:**

```bash
python3 devenv.py --env environments/test-1.toml start
```

**Options summary:**

| Flag | Default | Effect |
|---|---|---|
| `--env PATH` | `environments/dev.toml` | Environment TOML to use |
| `--no-ha` | off | Skip components with `ha_only = true` |
| `--delay SECONDS` | `1.0` | Pause between component starts |

## Namespace

All framework classes live in the `pubsub_itc_fw` namespace.

## License

Apache-2.0