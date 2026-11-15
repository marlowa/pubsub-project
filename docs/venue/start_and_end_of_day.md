# Starting and ending the trading day {#start_and_end_of_day}

**Status: agreed and built (section 5): each file records its trading day and is refused on another day, and `scripts/trading_day.py` carries out both procedures.** This note is step 3 of
[trading_day_plan.md](trading_day_plan.md). It lists every file the venue writes during a trading day, and sets out
the procedure that starts a day and the procedure that ends one
([BUG-0137](../bug_list.md#bug_0137)). The choices made are collected in the last section.

Principle V12 of [`design_principles.md`](../design_principles.md) states the rule these procedures carry out: every
trading day starts with none of the previous day's files. At the end of each day every component is stopped and
every file the venue wrote during the day is moved into an archive for that day. The WAL is the record of one
trading day (principle V2), and a Quill log never covers two days.

## 1. What exists today

- **No procedure starts or ends a day.** `scripts/devenv.py start` starts the components in the order the
  environment file gives (`[startup_order]`), and `devenv.py stop` stops them in the reverse order. Neither knows
  anything about a trading day.
- **A component tells a restart from a fresh start only by whether its files are present.** A sequencer that finds
  a WAL that is not empty treats its start as a restart. No file the venue writes records which trading day it
  belongs to, except the WAL, whose records carry a trading date.
- **The nearest thing to a start-of-day procedure is a developer's option.** `devenv.py start --fresh-wal
  --fresh-book` removes the WALs, the open-order regions, the lease promise records and the arbiters' statements
  before starting (`clear_durable_state`). It does not remove the matching engines' epoch files, the Quill logs,
  or the output captured from each component, so a run started this way still carries those from the run before.
- **The danger is a file left from an earlier day.** For example, yesterday's open-order region would make the
  matching engine judge itself absent for hours, cancel yesterday's orders, send the cancels to members, and halt
  the new day before it opens ([BUG-0137](../bug_list.md#bug_0137)).

## 2. Every file the venue writes during a trading day

The witness writes no file but its Quill log and its captured output, and the matching engine publisher keeps no
lease promise or epoch on disk. The list was built by reading every component's configuration and searching the source of every component and of
the framework for code that creates, writes or renames a file. The paths are those of the development environment
(`installed/environments/dev.toml`); each is a setting, and the other environments set their own.

### 2.1 Files that belong to one trading day

Every file here is moved into the day's archive at the end of the day. Only those a component reads when it starts
stop a new day from starting if they are left from another day: the WALs and the files beside them, the open-order
regions, the epoch files, the lease promise records and the arbiters' statements. The others cannot be taken for the
new day's, and a venue that refused to start because an old Quill log was lying about, or because a credentials file
it is about to overwrite was present, would be refusing for nothing (section 3, step 2).

| Written by | File | What it holds | Setting |
|---|---|---|---|
| Each sequencer | the WAL directory: `wal_NNNNNN.log` segments and `snapshot.bin` | Every command and report of the day, in sequence | `wal_directory` (`var/sequencer_wal`, `var/sequencer_secondary_wal`) |
| Each sequencer | `epoch.state`, in the WAL directory | The highest leadership epoch the sequencer has recorded (`EpochStore`, through `ReplicatedWal`) | follows `wal_directory` |
| Each sequencer | `lease_promise.state`, in the WAL directory | The promise the sequencer has made to an arbiter about its lease (`LeasePromiseStore`) | follows `wal_directory` |
| Each sequencer | `engine_routed_at`, in the WAL directory | The position up to which orders have been routed to a matching engine (`EngineRoutedStamp`) | follows `wal_directory` |
| Each matching engine publisher | its own WAL directory, copied from the sequencer's | The records the publisher has taken from the sequencer, from which it serves its topics | `wal_directory` (`var/mep_primary_wal`, `var/mep_secondary_wal`) |
| Each matching engine | the open-order region | Every order open on the engine's book, and whether trading is halted | `order_book_region_path` (`var/matching_engine_open_orders.region` and the secondary's) |
| Each matching engine | the region with `.unusable` added | A region the engine found damaged at start-up, moved aside so that it can be examined | follows `order_book_region_path` |
| Each matching engine | the epoch file | The highest leadership epoch the engine has recorded | `epoch_state_file` (`var/matching_engine_epoch.state` and the secondary's) |
| Each matching engine | the epoch file with `.lease_promise` added | Its lease promise | follows `epoch_state_file` |
| Each arbiter | the lease promise file | The promise of its vote | `lease_promise_file` (`var/arbiter_primary_lease_promise.state` and the secondary's) |
| Each arbiter | the lease promise file with `.component_statements` added | The statements component leaders have made about whether their followers may lead (`ComponentStatementStore`) | follows `lease_promise_file` |
| Each FIX order gateway, when capture is turned on | the capture file | Every byte received and sent on every FIX session, with times | `fix_capture_file` (`fix_capture.bin`); emptied at each start |
| Every component | its Quill log, and, for the components configured to rotate it, the earlier files (`max_file_size`, `max_backup_files`) | What the component did, at the levels configured | `log_dir` (`installed/log`), one file per component and trading day, such as `sequencer_primary.20261116.log` |
| Every component | the captured output of the process | What the process wrote to its standard output and standard error | `log_dir`, `<component>.<trading day>.stdout` |
| The start-of-day procedure | the run manifest | The trading day, the schedule and the release the day runs with (section 3) | `var/run_manifest.toml` under the installation |
| The start-of-day procedure | `credentials.toml`, exported from the database | Every member's stored SCRAM credentials, as the authentication services load them, with each change the administration service sends during the day | `credentials_file` |

Each store that replaces its file safely (`EpochStore`, `LeasePromiseStore`, `ComponentStatementStore`, and the
authentication service's credentials) writes a file with `.tmp` added and renames it over the old one. A process
stopped between the two leaves the `.tmp` file behind, and it belongs to the day like the file beside it.

`credentials.toml` is in this table, although members' credentials outlive the day, because the file is a copy:
the database holds the credentials, the start-of-day procedure exports a fresh copy every day, and the day's copy
is archived so that it shows exactly what the authentication services ran with.

### 2.2 Files that outlive the day

These are configuration and secrets, written when the venue is deployed or administered, and are not archived:

- every component's configuration, under `installed/etc`, and the environment files;
- the TLS certificates and keys;
- the matching engine's identifier secret (`identifier_secret_file`), which both engines must share;
- the administration service's own users (`AdminUserStore`) and its setup secret (`SetupSecret`).

### 2.3 Files that belong to the processes running, not to the day

These are written under `run_dir` (`installed/run`) and describe the processes running now, not the trading
day: each component's launcher state (`<component>.launcher.state`, written by `scripts/launch.py`), the
register of processor cores in use (`pubsub_cpu_registry` and its lock file), the core layout (`cpu_layout.toml`)
and the background tier's list. `devenv.py start` already removes the core register and its lock before
starting anything. They are not archived, and the end-of-day procedure leaves `run_dir` as `devenv.py stop`
leaves it.

### 2.4 Files that are not the venue's

Prometheus and Grafana are optional tools outside the venue, and keep their own data. The FIX test client's
captures belong to the person testing. Neither is archived with the day.

## 3. The start-of-day procedure

The procedure is a script run by an operator, or by a scheduler on the machine, before trading. It is given the
trading day `D` and the name of the file of the day's times for each phase. It does not decide `D` itself.

1. **Check that nothing is running.** No process of the installation may be running. The procedure checks each one
   by its process identifier, not by the exit status of a stop command, and stops if any is found.
2. **Check that no file from another day is present** that a component reads when it starts: the WALs and the files
   beside them, the open-order regions, the epoch files, the lease promise records and the arbiters' statements. The other files in section 2.1 cannot be taken for the new day's, so they do not stop it:
   a Quill log or captured output has its day in its name, the credentials file is exported again at step 4 before any
   component starts, a FIX capture file is emptied as its gateway starts, and a region set aside as unusable is never
   read again. The deployment writes a credentials file too, so a check of every file would refuse the first day after
   every deployment. The end-of-day procedure archives them all the same. For the files checked:
   - if none is present, the day starts fresh;
   - if the run manifest is present, names `D` and says the day's inputs are complete, this is a restart during
     day `D`, not a start of day. The procedure goes to step 6 and starts the components on their files, as a
     restart after a failure does now. It does not export the inputs again: `credentials.toml` is then the
     authentication services' copy for the day, holding every change sent to them since the start. A schedule
     file given to a restart must give the same times as the manifest; the times are changed during the day by
     a `TradingScheduleRequest`, not by restarting;
   - if the run manifest is present, names `D` and says the day's inputs are not complete, an earlier start of
     day `D` stopped before it had them. No component has started, so the procedure carries on from step 3 and
     repeats everything from there;
   - if the run manifest names another day, or any file checked is present with no run manifest, the procedure
     refuses. It names every file it found, with the day each belongs to where the procedure reads it: the day a
     small text file records in its first field, or the day a run manifest names. The day a WAL or a region holds is
     read by the component that uses it. The procedure says that the end-of-day procedure for that day has to be run
     first, and never deletes or moves anything itself.
3. **Write the run manifest**, `var/run_manifest.toml`: the trading day `D`; the schedule, as the moments in UTC
   that the venue's local times for `D` come to; the release installed (from `release.json`); the time the
   manifest was written; and `inputs_complete = false`. Every later step, and the start-of-day request, takes `D`
   and the schedule from here. The manifest is written first so that every file written after it is known to
   belong to day `D`.
4. **Export the day's inputs from the database, and check them.** Today the inputs are the members' credentials,
   as `devenv.py start` already exports them (`export_credentials`). The reference data joins this step when it
   exists ([trading_day_and_reference_data.md](trading_day_and_reference_data.md)). Each input is written under a
   temporary name and renamed into place only once it is complete. The procedure then reads each one back, checks
   that it parses and that it holds as many entries as the database query returned, and only then rewrites the
   manifest with `inputs_complete = true`. If the export or the check fails, the procedure stops with the manifest
   still saying the inputs are not complete, so the next attempt repeats this step. The venue never starts on
   yesterday's copy or on a partial one.
5. **Check that the manifest says the inputs are complete.** Starting the components is allowed only from here.
6. **Start the components** in the order the environment file gives, each with `--trading-day D` on its
   command line. A component that finds one of its files in section 2.1 written on a day other than `D`
   refuses to start, naming the file and the day ([BUG-0137](../bug_list.md#bug_0137), item 4). This check is the
   one that holds even when the procedure is not used.
7. **Ask for the start of day.** The procedure sends the start-of-day request, with `D` and the schedule, to both
   sequencers over their listener for operators, as [trading_day_plan.md](trading_day_plan.md) section 3.2 sets
   out. The sequencer that does not lead answers so. The procedure repeats the request until the leader answers
   that it has written the start of day, or that the venue is already in it or past it. Repeating is harmless, so
   a restart during the day runs this step too and is answered that the venue is already past it.
8. **Report and stop.** The procedure reports the trading day, the phase the venue is in, and the time of each
   scheduled phase change, and exits. Nothing outside the venue has to run for the day to proceed: the leading
   sequencer moves the venue from one phase to the next by the schedule in its WAL.

Logging on is allowed in every phase, so members may connect as soon as the gateways are up at step 6.

## 4. The end-of-day procedure

The procedure is run once the venue has entered `eod`. It does not move the venue on by itself: closing early is an
operator's request to the sequencer, made on purpose, not a side effect of ending the day.

1. **Check that the venue is in `eod`** for the day named in the run manifest. The procedure asks the leading
   sequencer for its state with a `TradingStateQuery` (234), which writes nothing and is answered with a
   `TradingPhaseAnswer` whose outcome is `StateOnly`. If no sequencer leads, for instance because the venue stopped
   after entering `eod`, the procedure refuses, and the venue is started again on the day's files, as a restart, so
   that it can be asked. If the venue is in an earlier phase, the procedure refuses and says which phase it is
   in and when `eod` is scheduled.
2. **Wait for the order activity recorder** to report that every event up to the `eod` event is confirmed by the
   external messaging system. The recorder is not built, and how its report reaches this procedure is designed with
   it ([trading_day_plan.md](trading_day_plan.md), decision 4). Until then this step is skipped.
3. **Stop every component**, in the reverse of the order they were started in, and check that each process has
   exited by its process identifier. If any has not exited, the procedure stops and archives nothing, because a
   running component may have the files open or mapped.
4. **Archive every file in section 2.1.** The procedure moves each one into `<archive_dir>/<D>/<machine>/`, where
   `archive_dir` is a setting in the environment file,
   keeping its path below the installation, so that the files from the two machines of a pair stay apart. Before
   removing anything from its place, it writes a listing of every file with its size and a SHA-256 checksum, and
   checks each archived copy against it. A move within one filesystem is a rename; a move to another filesystem is
   a copy, checked and synced, followed by the removal of the original.
5. **Report.** The procedure reports where the archive is, how many files and bytes it holds, and that the
   installation now holds no file of the day. The next day's start-of-day procedure finds nothing at step 2.

The archive is the day's record. How long archives are kept, and where they are sent, is an operational matter
outside the venue.

Each machine archives its own files. In the development environment every component runs on one machine, so one
run of the procedure archives everything; in an environment with two machines the procedure runs on each, and
step 3 on each machine stops only the components on that machine. That case is not handled yet: the machine that
runs the procedure second may find no sequencer running to answer step 1, and refuses
([BUG-0137](../bug_list.md#bug_0137)).

## 5. What has to be built

This is the work for step 9 of [trading_day_plan.md](trading_day_plan.md), listed here so that the procedures can be
judged against it:

- **Each file in section 2.1 records its trading day**, and each component refuses at start-up a file from a day
  other than the one it was given. The FIX capture file needs no check, because the gateway empties it at each
  start. **Built:**
  - the small text files (the epoch files, the lease promise records and the arbiters' statements) begin with the
    field `trading_day=YYYYMMDD` (`TradingDayOfFile.hpp`), and the sequencer's record of the last engine routed to
    holds the day after its time;
  - the open-order region holds the day in its header, in the fourth of the values its owner keeps;
  - the sequencer reads the day from the start of day at the head of its WAL, and the publisher from the first
    record of its copy;
  - a file that records no day is refused as one from an unknown day. Each refusal is logged at Error, beginning
    "refusing to start for trading day D", naming each file and the day it belongs to, and the file is left
    exactly as it was. Scenario 114 of `ha_test.py` checks every one of these.
- **The trading day is passed to each component** as `--trading-day D` on its command line, and each component
  logs it as it starts. **Built**, for every component the development tools, the test harness and the performance
  scripts start.
- **`devenv.py start --fresh-wal` removes the matching engines' epoch files.** **Built.**
- **The procedures themselves**, as one script, `scripts/trading_day.py`, with the commands `start` and `end`,
  using the same reading of the environment file as `devenv.py`, and the same functions to start and stop the
  components. `devenv.py start --fresh-wal --fresh-book` remains for development runs, and clears what a test run
  leaves, including regions set aside as unusable, so that `trading_day.py start` finds none of it. **Built.** The
  question `end` asks, `TradingStateQuery`, is new: no request that existed could ask for the phase without the risk
  of changing it.
- **The Quill logs** carry the trading day in their file names, such as `sequencer_primary.20261116.log`, so that
  two days can never share a file. The captured standard output files are named in the same way. **Built**, for the
  components `devenv.py` and `trading_day.py` start. The test harnesses (`ha_test.py`, `perf_run.py` and
  `callgrind_run.py`) start their own components with logs named without a day, and clear them before each run; the
  procedures leave those files alone.
- **The `archive_dir` setting** in each environment file, `installed/archive` in the development environment.
  **Built.**

## 6. Decisions

1. **A component is given the trading day on its command line**, as `--trading-day D`. It is visible in the
   process list, the component logs it as it starts, and it works the same way for a component restarted during
   the day. A component never computes the day from its clock (R-0164).
2. **Each Quill log's file name carries the trading day.** Quill appends to a log file that already exists, so a
   name without the day would let a log left from an earlier day gain the new day's lines. With the day in the
   name two days never share a file, whether or not anything was archived, and the name says which day a log
   covers.
3. **The archive goes where the `archive_dir` setting in the environment file says**, `installed/archive` in the
   development environment.
