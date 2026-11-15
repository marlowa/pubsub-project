# Starting and ending the trading day {#start_and_end_of_day}

**Status: the start of the day is agreed and built (sections 3 and 5): each file records its trading day and is refused on another day, and `scripts/trading_day.py start` starts a day. How the day ends is agreed in shape and open in its details (section 4): each component shuts itself down and archives its own files when it receives the end of day.** This note is step 3 of
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

## 4. Ending the trading day

**Status: the shape is agreed; the decisions in section 4.2 are open except where they say otherwise, and none of it is built.** What is built today
is a procedure run from outside the venue, described in section 4.3, which this design replaces.

### 4.1 How the day ends

The end of day is made of trading phases, like the start of day, and reaches every component in the same way. The
leading sequencer enters it at the time the day's schedule gives, or when an operator asks for it with
`trading_phase.py end-of-day`, which a scheduler such as cron can run at a fixed time just as the start of day is run.
Entering it is a `TechnicalEvent` (153) in the WAL. From there it reaches each component by the routes that exist
today for every phase:

- the matching engines apply it from the WAL;
- the follower sequencer copies it into its own WAL;
- the matching engine publishers read it from their copy of the WAL and publish it on the `technical_events` topic;
- the leading sequencer tells each gateway in an `OrderAcceptance` (127), which carries the trading day and the phase.

The end of day is two events, both written into the WAL by the leading sequencer and both reaching every component
by these routes (decision 3):

- **the end-of-day begin** (`EndOfDayBegin`, the phase called `EndOfDay` today) means the day's trading is over.
  Nothing more is traded, the copies of the WAL are completed, and no component shuts down;
- **the end-of-day end** (`EndOfDayEnd`, a new phase) is written by the leading sequencer only once every copy of the
  WAL is complete, and means "shut down and archive now".

**Each component, on receiving the end-of-day end, shuts itself down in an orderly way, and archiving its own files
is part of that shutdown.** It finishes what it is doing, closes its WAL, its regions and its other files, and moves
every file in section 2.1 that it wrote into `<archive_dir>/<D>/<machine>/`, keeping the file's path below the
installation. It writes a listing of every file it moves, with its size and a SHA-256 checksum, before removing
anything from its place, and checks each archived copy against the listing, as section 4.3 describes for the
procedure built today. Then it exits.

Three things follow from this:

- **There is no separate archiving step.** Nothing outside the venue has to run for the day to end, just as nothing
  has to run for the day to proceed once it has started (section 3, step 8).
- **No component has to ask whether the day ended.** Receiving the end-of-day end is the evidence that it ended. A
  component never archives on any other evidence, so a venue that stops during the day, by a crash or by an operator
  stopping it, leaves every file in place for a restart.
- **No machine depends on another.** Each component archives the files it wrote, on the machine it runs on, so
  `archive_dir` need not be shared between machines, and the components can be spread over as many machines as the
  environment file says. The pre-production environment, `environments/preprod.toml`, uses ten.

### 4.2 What the design has to settle

Each of these is open unless it says otherwise. Where a choice is suggested, it is a suggestion, not a decision.

Two senses of "follower" appear below and are kept apart. **The follower sequencer** is the instance of the
sequencer pair that does not lead; there is at most one. **The WAL's receivers** are every component the leading
sequencer sends the WAL's records to as it writes them (`WalFollowerService`, and `ReplicatedWal` for the follower
sequencer): the follower sequencer, both matching engines and both matching engine publishers. Only the leading
sequencer writes the WAL. The follower sequencer and each publisher keep a copy of it; the matching engines keep none,
and apply each record as it arrives.

1. **Every component subscribes to the end of day.** Agreed. It is what tells a component that from now on the
   departure of another component is expected (decision 2). The matching engines, the publishers, the follower
   sequencer and the gateways receive it today, by the routes in section 4.1. The arbiters, the witness, the
   authentication services and the administration service receive no phase change at all, and have to subscribe to
   the `technical_events` topic, which exists to carry the phase to whatever subscribes. A component that subscribes
   depends on the publishers being up, so the publishers have to be among the last to stop. The end of day is two
   events (decision 3), and a component subscribes to both.

2. **Other components leaving must not be taken for a failure.** Agreed, and it follows from decision 1. During the
   day, a component that loses another treats the loss as a fault and acts on it: a follower sequencer whose leader
   stops asks the arbiters to let it lead; a sequencer whose arbiters stop loses its lease; a gateway whose
   sequencer stops tries to connect again, and tells its members trading is unavailable; a matching engine whose
   sequencer stops waits for one. Once a component has received the end of day, each of these is expected: the
   departure is logged at Info and leads to no promotion, no halt and no alarm. What is still open is the order of
   shutdown, so that a component leaves only once nothing still running depends on it. One order that fits the
   dependencies is: the gateways first, then the administration service and the authentication services, then the
   matching engines, then the follower sequencer, then the leading sequencer, then the publishers, and the arbiters
   and the witness last, which is close to the reverse of the start order in `[startup_order]`. How each component
   knows its turn has come is part of the decision: it could wait for those before it to disconnect, or be told by
   the leading sequencer.

3. **Nothing is archived before the day's record is complete.** Two things have to be true before the leading
   sequencer closes its WAL and archives it.
   - **Every report has arrived.** The matching engines never write the WAL, but what they do is recorded in it:
     each engine sends its execution reports to the sequencer, which writes them into the WAL. At the close each
     engine cancels every open order and sends a report of each cancel, and the end-of-day begin is written only once
     the engine has acted on the close (`scripts/trading_phase.py`). That ordering looks to be enough, but it is to be
     confirmed that no report can reach the sequencer after the end-of-day begin is written. A report that did would be
     missing from the archived WAL, and would never reach the member.
   - **Every copy is complete.** The follower sequencer and both publishers hold every record up to the end-of-day
     begin, and the order activity recorder has reported that every event up to it is confirmed by the external
     messaging system ([trading_day_plan.md](trading_day_plan.md), decision 4; the recorder is not built).

   **Agreed: the end of day is two events.**
   - **The end-of-day begin** (`EndOfDayBegin`) is the phase called `EndOfDay` today, entered at the time in the
     schedule or on an operator's request. It means the day's trading is over: nothing more is traded, and the
     copies of the WAL are completed. No component shuts down on it.
   - **The end-of-day end** (`EndOfDayEnd`) is a new phase, the last of the day. The leading sequencer writes it
     once the follower sequencer and both publishers have confirmed they hold every record up to the end-of-day
     begin, and the order activity recorder has confirmed every event up to it, or once the limit of time in
     decision 4 has passed. It means "shut down and archive now". It is a record in the WAL like the first, so every
     component receives it by the same routes, and both sequencers' WALs end with it.

   The moment every copy is complete is then a single record that every component sees, rather than a condition
   each component judges for itself. Why one event is not enough is the first case of decision 4: the leader tells
   the gateways of a phase as soon as it has written it, before the follower sequencer holds it, so a single event
   could set components shutting down on a record that a change of leader then loses.

4. **What happens when the two sequencers' WALs differ at the end of day.** The leading sequencer sends each record
   to the follower sequencer and waits for it to confirm before sending the execution reports that depend on it to
   the gateways, so the follower sequencer holds every record whose reports a member has seen. What it can lack is a
   short tail of records the leader has written and has not yet seen confirmed. **A change of phase is not held in
   this way:** the leader tells the gateways of it in an `OrderAcceptance` as soon as it has written it
   (`SequencerThread.cpp`, after each record is written). These are the cases:
   1. **The leader writes the end-of-day begin and stops before the follower sequencer has it.** The gateways have
      already been told of it. The arbiters promote the
      follower sequencer, whose WAL ends in the closed phase. If the end of day was scheduled, the new leader enters
      it at once, because its time has passed, and writes its own end-of-day begin. If an operator asked for it,
      the new leader stays closed until asked again, and the gateways are told the venue is closed after being told
      the day ended. Had one event made components shut down, the gateways would already be leaving. With two events
      nothing shuts down on the end-of-day begin, and the end-of-day end is written only once the follower sequencer
      holds every record before it, so a change of leader cannot lose it, except after the limit of time in cases 4
      and 5. What is left is that members can be told
      of a phase and then of the phase before it, which happens for every phase, not only this one
      ([BUG-0249](../bug_list.md#bug_0249)).
   2. **The leader writes the end-of-day end and shuts down before the follower sequencer holds it.** The follower
      sequencer would then hold every record but the last, and, never receiving the end-of-day end, would not shut
      down at all. The leader must not shut down until the follower sequencer has confirmed the end-of-day end
      itself; after that nothing more is written, and the two archived WALs are the same.
   3. **No follower sequencer is running at the end of day,** because the venue has been running without its
      standby. The leader archives alone. The follower sequencer's machine still holds the day's WAL up to the point
      the follower stopped, and that machine is dealt with by decision 6.
   4. **The follower sequencer is still being caught up** when the end-of-day begin arrives, after a restart late in the
      day. The leader waits for it to finish, but only for a limit of time, after which it treats the follower
      sequencer as absent, as in case 3. The limit is to be decided.
   5. **The follower sequencer holds a record, and the leader never receives its confirmation,** because the
      connection broke. The leader waits for the same limit and then goes on without the confirmation: when the
      record is the end-of-day begin or one before it, the leader writes the end-of-day end; when it is the
      end-of-day end, the leader shuts down and archives. Both WALs may be complete, but the
      leader cannot know it, and says so in its log at Warning.

5. **Each component keeps a file of its last trading state, and the launcher does not restart a component that left
   at the end of day.** The launcher, `scripts/launch.py`, restarts its component whenever it exits, for any reason,
   because during the day any exit is a loss of service. Suggested:
   - **the file.** Each component keeps a small text file under the installation, beginning with the trading day like
     the other small files: `trading_day=20261116 EndOfDayEnd`, written under a temporary name and renamed into place
     whenever the phase changes. It is an ordinary file, not shared memory, and the venue never writes to a public
     directory such as `/dev/shm`. A component reads it first when it starts. If it says the end-of-day end of the day the
     component was started for, the component has no work to do: it archives anything of the day still present and
     exits. This settles a component restarted after the end of day (decision 8);
   - **the exit status.** The file alone does not stop the launcher, which would start the component, see it exit,
     and start it again without end. A component that exits at the end of day does so with a status that means
     "stopped for the end of day", and the launcher, seeing it, exits too;
   - **the file outlives the day.** It is not archived at the end of day; the next one overwrites it. On starting day
     D+1, a file saying the end-of-day end of D shows the component finished D in good order. A file saying an earlier
     phase, or an older day, shows it missed the end of day, which is decision 6 found without being looked for. The
     start-of-day check of section 3, step 2 needs one exception for this file, since it is meant to come from the
     day before.

6. **A component that misses the end of day keeps its files.** A component that was not running when the end of day
   was entered, or whose machine could not be reached, never receives it, and its files stay where they are. Its
   last-state file says so (decision 5). The next day's start then refuses because of those files (section 3, step
   2), which is safe: nothing is lost and nothing is taken for the new day's. An operator then has to archive them
   by hand. The procedure built today, `trading_day.py end` (section 4.3), could be kept for exactly that, as the
   manual way to archive one machine's files, rather than as the normal way to end a day. By then nothing may be
   running to ask whether the day ended, so the evidence it asks for would be the operator's word, given as an
   option and recorded in the listing.

7. **Files a component cannot archive itself.** A component can move every file it opened. It cannot move the files
   something else holds open or wrote for it:
   - **the captured console output**, standard output and standard error, which the launcher holds open for as long
     as the process runs. Agreed: the next day's start moves it into the previous day's archive. By then the
     launcher has exited and closed it, and its name carries the day, so it can never be taken for the new day's;
   - **the run manifest and the exported `credentials.toml`**, written by the start-of-day procedure. The
     authentication services read and rewrite `credentials.toml` during the day, so they can archive it. The run
     manifest belongs to no component, so the next day's start can archive it with the console output.

8. **A component started during the end of day.** A component restarted after the end-of-day end was written finds
   it in its last-state file (decision 5) and does what it would have done had it been running. A component restarted
   between the two events takes part in completing the copies like any other. A component that also meets the
   end-of-day end in records it receives while being caught up has to act on it once, after it has caught up,
   and not part of the way through.

### 4.3 What is built today

`scripts/trading_day.py end` is a procedure run by an operator on a machine, after the venue has entered the end
of day. It does not move the venue on by itself.

1. **It checks that the venue is in the end of day** for the day named in the run manifest, by asking the leading
   sequencer for its state with a `TradingStateQuery` (234), which writes nothing and is answered with a
   `TradingPhaseAnswer` whose outcome is `StateOnly`. If no sequencer leads, or the venue is in an earlier phase,
   it refuses, and says which phase the venue is in and when the end of day is scheduled.
2. **It would wait for the order activity recorder** to report that every event up to the end of day is confirmed.
   The recorder is not built, so this step is skipped.
3. **It stops every component on the machine**, in the reverse of the order they were started in, and checks that
   each process has exited by its process identifier. If any has not, it archives nothing, because a running
   component may have the files open or mapped.
4. **It archives every file in section 2.1** into `<archive_dir>/<D>/<machine>/`, keeping each file's path below the
   installation; a file outside the installation goes under `outside_the_installation/` with its absolute path.
   Before removing anything from its place, it writes `listing.txt`, naming every file with its size and a SHA-256
   checksum, and checks each archived copy against it. A move within one filesystem is a rename; a move to another
   filesystem is a copy, checked and synced, followed by the removal of the original. An archive stopped part of the
   way through is finished by running the procedure again. The run manifest is moved last.
5. **It reports** where the archive is, how many files and bytes it holds, and that the installation holds no file
   of the day.

`scripts/trading_day_test.py` checks it on one machine. On more than one machine it does not work: the machine that
runs it after the sequencers' machines have stopped finds no sequencer to answer step 1, and refuses
([BUG-0137](../bug_list.md#bug_0137)). That is why the design in section 4.1 replaces it. Its listing, its checks of
each copy and its way of moving files across filesystems carry over to the archiving each component does.

The archive is the day's record. How long archives are kept, and where they are sent, is an operational matter
outside the venue.

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
  of changing it. The `end` command is to be replaced by each component archiving its own files (section 4).
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
