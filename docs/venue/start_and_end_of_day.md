# Starting and ending the trading day {#start_and_end_of_day}

**Status: the start of the day is agreed and built (sections 3 and 5): each file records its trading day and is refused on another day, and `scripts/trading_day.py start` starts a day. How the day ends is agreed and built (section 4): the end of day is two events, and the leading sequencer writes the second once every copy of the WAL is complete; when each component receives it, it shuts itself down, keeping a file of its last trading state, and its launcher archives its files (section 4.4). The order activity recorder, which the end of day would wait for, is not built.** This note is step 3 of
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
   - if the run manifest names another day, and no other file checked is present, that day ended in good order:
     every component archived its own files. The run manifest and `credentials.toml` belong to no component, so the
     procedure archives both into that day's archive, with a listing of their own, `listing.next_start.txt`, written
     and checked as the end-of-day procedure writes and checks its listing, and the day starts fresh (section 4.2,
     decision 7);
   - if the run manifest names another day and any other file checked is present, or any file checked is present
     with no run manifest, a component missed the end of day, and the procedure refuses. It names every file it found, with the day each belongs to where the procedure reads it: the day a
     small text file records in its first field, or the day a run manifest names. The day a WAL or a region holds is
     read by the component that uses it. The procedure says that the end-of-day procedure for that day has to be run
     first, and moves nothing: the run manifest stays, to name the day to `trading_day.py end`.
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

**Status: agreed and built.** The leading sequencer writes the end-of-day end once the follower sequencer and the
publishers hold every record up to the begin, or once the limit of time has passed, and refuses an operator's
request for it (decisions 3 and 4). When a component receives the end-of-day end it shuts itself down, keeping a
file of its last trading state, and its launcher archives its files (section 4.4). The procedure run from outside
the venue, `trading_day.py end` (section 4.3), is kept as the manual way to archive the files of a component that
missed the end of day.

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

**Each component, on receiving the end-of-day end, shuts itself down in an orderly way, and its launcher then
archives its files.** The component finishes what it is doing, closes its WAL, its regions and its other files, and
exits with a status that means "stopped for the end of day" (decision 5). Every component is started by its own
launcher, `scripts/launch.py`, which runs on the same machine, restarts the component whenever it exits during the
day, and holds open the file that captures the component's console output. On seeing that status, the launcher does
not restart the component. It closes the captured console output, and moves every file in section 2.1 that belongs
to its component, the captured console output among them, into `<archive_dir>/<D>/<machine>/`, keeping each file's
path below the installation. It writes a listing of every file it moves, with its size and a SHA-256 checksum,
named after its component, `listing.<component>.txt`, so that the launchers on one machine do not share a listing,
before removing anything from its place, and checks each archived copy against the listing, using the code that
`trading_day.py end` uses today (section 4.3). Then the launcher exits.

The archiving is done by the launcher and not by the component's own process for three reasons. The launcher acts
only after the component has exited, so no file it moves can still be open or mapped. It holds the captured console
output, which the component cannot move. And the code that lists, checks and moves the files exists once, in
Python, rather than being written again in C++ for the components built on the framework and in Java for the
administration service.

There is no order in which components shut down: each does so as soon as it receives the end-of-day end, with the two
exceptions in decision 2. Once the end is written, trading is over and every copy of the WAL is complete, so no
running component needs anything more from another, except to be told of the end itself.

Three things follow from this design:

- **There is no separate archiving step.** Nothing outside the venue has to run for the day to end, just as nothing
  has to run for the day to proceed once it has started (section 3, step 8).
- **No component has to ask whether the day ended.** Receiving the end-of-day end is the evidence that it ended. A
  launcher never archives on any other evidence, so a venue that stops during the day, by a crash or by an operator
  stopping it, leaves every file in place for a restart.
- **No machine depends on another.** Each launcher archives the files of its own component, on the machine they both
  run on, so `archive_dir` need not be shared between machines, and the components can be spread over as many
  machines as the environment file says. The pre-production environment, `environments/preprod.toml`, uses ten.

### 4.2 What the design has to settle

Each of these is agreed, except the two values the status of section 4 names.

Two senses of "follower" appear below and are kept apart. **The follower sequencer** is the instance of the
sequencer pair that does not lead; there is at most one. **The WAL's receivers** are every component the leading
sequencer sends the WAL's records to as it writes them (`WalFollowerService`, and `ReplicatedWal` for the follower
sequencer): the follower sequencer, both matching engines and both matching engine publishers. Only the leading
sequencer writes the WAL. The follower sequencer and each publisher keep a copy of it; the matching engines keep none,
and apply each record as it arrives.

1. **Every component subscribes to the end of day.** The end of day is two events (decision 3), and every component
   receives both. The matching engines, the publishers, the follower sequencer and the gateways receive every phase
   today, by the routes in section 4.1. The arbiters, the witness, the authentication services and the administration
   service receive no phase change at all, and have to subscribe to the `technical_events` topic, which exists to
   carry the phase to whatever subscribes. The end-of-day end is what tells a component that from then on the
   departure of another component is expected (decision 2).

2. **Other components leaving at the end of day must not be taken for a failure, and there is no order of
   shutdown.** During the day, a component that loses another treats the loss as a fault and acts on it: a follower
   sequencer whose leader stops asks the arbiters to let it lead; a sequencer whose arbiters stop loses its lease; a
   gateway whose sequencer stops tries to connect again, and tells its members trading is unavailable; a matching
   engine whose sequencer stops waits for one.

   **From the end-of-day end onwards, and not before, each of these is expected:** the departure is logged at Info
   and leads to no promotion, no halt and no alarm. Between the end-of-day begin and the end-of-day end, no component
   shuts down, so a component that leaves in that interval has failed, and is treated exactly as it would be during
   the day. This matters because the venue still has work to do in that interval. If the leading sequencer stops
   after writing the begin, the follower sequencer has to be promoted so that the end is written at all; were the
   departure taken as expected, nothing would ever write the end, and no component would ever shut down.

   **Each component shuts itself down as soon as it receives the end-of-day end,** in no order relative to the
   others. An order of shutdown would be needed only if a component still running depended on one that had left.
   Once the end is written, trading is over and every copy of the WAL is complete, so the only thing any component
   still needs from another is to be told of the end. Two kinds of component do that telling, and they are the two
   exceptions:
   - **the leading sequencer** tells the follower sequencer, both matching engines, both publishers and the
     gateways. It does not shut down until the follower sequencer has confirmed that it holds the end-of-day end
     (decision 4, case 2);
   - **each publisher** tells the arbiters, the witness, the authentication services and the administration service,
     by publishing the end on the `technical_events` topic. A publisher cannot know whether a subscriber has received
     a message, so after publishing the end it stays up for a fixed time, so that a subscriber reconnecting at that
     moment still receives it. The time is a setting of the publisher's configuration, 10 seconds. A
     subscriber that still misses the end keeps its files (decision 6).

   The gateways are told of the end as soon as the leading sequencer has written it, before the follower sequencer
   holds it ([BUG-0249](../bug_list.md#bug_0249)). A gateway can therefore shut down on an end that a change of
   leader then loses. That does no harm: the gateway was about to shut down in any case, its files are the same
   either way, and the new leader writes an end of its own.

3. **Nothing is archived before the day's record is complete.** Two things have to be true before the leading
   sequencer closes its WAL and archives it.
   - **Every report has arrived.** The matching engines never write the WAL, but what they do is recorded in it:
     each engine sends its execution reports to the sequencer, which writes them into the WAL. At the close each
     engine cancels every open order and sends a report of each cancel. A report that reached the sequencer after the
     end-of-day begin would be missing from the archived WAL, and would never reach the member. **No report that the
     WAL does not already hold can reach the sequencer after the begin,** for four reasons, found by reading the code:
     1. Before writing the begin, the leading sequencer asks the matching engine that orders are routed to how far it
        has got, and writes the begin only when the engine has acted on the close, holds no report it has not sent,
        and every report it has sent is in the WAL (`TradingPhaseRequests::move_on` in
        `applications/sequencer/TradingPhaseRequests.cpp`).
     2. From the close onwards, the sequencer itself refuses every member's new order and cancel, whatever the gateway
        believed when it passed the command on, and answers with a `CommandRefused` sent straight to the gateway. No
        member's command reaches an engine after the close, so none produces a report.
     3. The commands the sequencer writes for itself produce no report after the close. Cancelling the orders of a
        session that has disconnected is not carried out from the close onwards. The `CancelBook` written when the venue
        cannot recover by itself finds the book empty, because the close cancelled every order. A `VoidCommands` for the
        new orders held at that halt finds none, because none has been accepted since the close. A matching engine's
        request to void a command it died applying is declined, because the engine orders are routed to has applied
        and answered every command before the close.
     4. If the matching engine orders are routed to stops after the begin and the other engine takes the lead, the new
        one sends only the reports it kept that come after the latest report in the WAL, and there are none.

     The one kind of report that can still arrive is a repeat of one the WAL already holds. A matching engine that
     restarts replays the day's WAL, and until it is told whether it leads it holds the reports the replay produces,
     up to 50,000 of them. Beyond that it sends each as it is produced, marked as a possible repeat, and the leading
     sequencer writes it into the WAL without checking whether the WAL already holds it
     ([BUG-0250](../bug_list.md#bug_0250)). Nothing is missing from the archive because of this: the original of every
     such report is before the begin, and a repeat written after the begin is before the end, which the follower
     sequencer confirms it holds before the leading sequencer shuts down.
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

   **How the leading sequencer learns that each copy is complete.** The follower sequencer acknowledges every record
   it writes (`WalAck`), so the leading sequencer always knows the last record it holds. A publisher acknowledges
   nothing as it goes. So after the begin, the leading sequencer asks each publisher, once a second, for the last
   record its copy of the WAL holds (`WalHeldRequest`), and the publisher answers on the same connection
   (`WalHeldReply`). This costs nothing during the day, and works after a change of leader, because the new leader
   simply asks again. An answer from a publisher whose connection has since gone no longer counts.

   **How many publishers it waits for** is the setting `[end_of_day] publishers` in the sequencer's configuration.
   `deploy.py` fills it in by counting the publishers the environment starts, so that it cannot disagree with them:
   two in the development environment, one there with high availability off, and none in the pre-production,
   production and test environments, which run no publisher. A publisher that is down or restarting is therefore
   waited for, up to the limit of time in decision 4, rather than left out.

   **Only the leading sequencer writes the end-of-day end.** An operator who asks for it is refused, with the outcome
   `WrittenByTheVenue`, and nothing is written; a schedule that names it is refused. Otherwise an operator could make
   every component shut down before the copies were complete. The operator's command `trading_phase.py end-of-day`
   and the schedule's key `end_of_day` keep their names, and mean the end-of-day begin, the only part of the end of
   day an operator can ask for or schedule. The end-of-day end carries the reason `EveryCopyComplete`, or
   `CopiesNotCompleteInTime` when the limit of time ran out, and in that case the leading sequencer says at Warning
   which copy was incomplete.

   **A member sees no difference between the two events.** In both, a member's new order or cancel is refused with
   the reason `TradingDayEnded`, and the gateways report the trading session as closed, with the text "the end of
   day".

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
      standby. The leader writes the end-of-day end without the follower sequencer's confirmation, shuts down, and its
      launcher archives its files. The follower sequencer's machine still holds the day's WAL up to the point the
      follower stopped, and that machine is dealt with by decision 6.
   4. **The follower sequencer is still being caught up** when the end-of-day begin arrives, after a restart late in the
      day. The leader waits for it to finish, but only for a limit of time, after which it treats the follower
      sequencer as absent, as in case 3. **The limit is a setting of the sequencer's configuration,
      `[end_of_day] longest_wait_for_copies_seconds`, with a value for each environment.** It is counted from the
      moment the leading sequencer first finds the venue in the end-of-day begin, so a new leader waits the whole time
      again. Waiting too long costs little, because nothing is trading and the machines merely shut down
      later; giving up too early leaves the follower sequencer's archived WAL incomplete, and the leading sequencer's
      WAL the only full record of the day. **Every environment sets 600 seconds.** In the environments other than
      development the figure is provisional, and says so in the environment file, until a figure for each is chosen.

      **Measured on the development machine, 16 November 2026,** in its power-saving configuration, so the figures
      are lower bounds. 1.8 million orders were sent at 20,000 a second, which wrote 3,600,008 records, 1.6 GB,
      about 450 bytes a record: each order and its acknowledgement. The follower sequencer was stopped, its WAL set
      aside, and started again with none, so that it had to be sent the whole WAL:
      - the leader sent it 3,180,914 records in 10.05 seconds, about 316,000 a second, after a first connection had
        sent the first 419,098;
      - at the moment that catch-up ended, the leader's lease ran out and it took the lead again at a later epoch,
        which started the same catch-up again ([BUG-0252](../bug_list.md#bug_0252)); the second sent 3,328,535 records
        in 18.9 seconds, about 176,000 a second, while a matching engine was also being caught up.

      At these rates a WAL of 10 million records takes 30 to 60 seconds to send. A day of 50 million orders and as
      many cancels, with a report of each, is about 200 million records, which would take 11 to 19 minutes.
   5. **The follower sequencer holds a record, and the leader never receives its confirmation,** because the
      connection broke. The leader waits for the same limit and then goes on without the confirmation: when the
      record is the end-of-day begin or one before it, the leader writes the end-of-day end; when it is the
      end-of-day end, the leader shuts down, and its launcher archives its files. Both WALs may be complete, but the
      leader cannot know it, and says so in its log at Warning.

5. **Each component keeps a file of its last trading state, and the launcher does not restart a component that left
   at the end of day.** The launcher, `scripts/launch.py`, restarts its component whenever it exits, for any reason,
   because during the day any exit is a loss of service. Agreed:
   - **the file.** Each component keeps a small text file in the venue's run directory, the directory named by
     `[paths] run_dir` in the environment file, which also holds the component's process identifier file. The file
     begins with the trading day like the other small files: `trading_day=20261116 EndOfDayEnd`, written under a temporary name and renamed into place
     whenever the phase changes. It is an ordinary file, not shared memory, and the venue never writes to a public
     directory such as `/dev/shm`. A component reads it first when it starts. If it says the end-of-day end of the day the
     component was started for, the component has no work to do: it exits at once with the status below, and its
     launcher archives whatever of the day's files is still present. This settles a component restarted after the
     end of day (decision 8);
   - **the exit status.** The file alone does not stop the launcher, which would start the component, see it exit,
     and start it again without end. A component that exits at the end of day does so with status 3, which means
     "stopped for the end of day" (`STOPPED_FOR_THE_END_OF_DAY` in `scripts/launch.py`). The launcher, seeing it,
     does not restart the component and counts no interruption: it archives the component's files (section 4.1),
     and exits. **Built in the launcher:** it is given the environment file and the trading day with
     `--environment` and `--trading-day`, which `devenv.py --supervised` passes, and finds the component's files with
     `files_of_the_component` in `scripts/trading_day.py`. No component exits with status 3 yet, because none shuts
     itself down yet;
   - **the file outlives the day.** It is not archived at the end of day; the next one overwrites it. On starting day
     D+1, a file saying the end-of-day end of D shows the component finished D in good order. A file saying an earlier
     phase, or an older day, shows it missed the end of day, which is decision 6 found without being looked for. The
     start-of-day check of section 3, step 2 needs one exception for this file, since it is meant to come from the
     day before.
   - **where a component finds it.** The file's path is a setting of each component's configuration, which the
     deployment fills in from `[paths] run_dir`. A component started by a test on a copy of a day's files, such as the
     check in `ha_test.py` that runs the matching engines on a copy of the WAL, is given a path of its own, so that it
     neither reads nor overwrites the venue's file. A copy of a WAL that already holds the end-of-day end makes such a
     component shut down once it has caught up, as decision 8 requires of any component, and the check takes its exit
     with status 3 at that point as a normal finish. Agreed with Andrew on 19 November 2026, in preference to a
     setting under which a component ignores the end of day, which would put a mode used only by tests into the
     components;

6. **A component that misses the end of day keeps its files.** A component that was not running when the end of day
   was entered, or whose machine could not be reached, never receives it, and its files stay where they are. Its
   last-state file says so (decision 5). The next day's start then refuses because of those files (section 3, step
   2), which is safe: nothing is lost and nothing is taken for the new day's. An operator then has to archive them
   by hand. **The procedure built today, `trading_day.py end` (section 4.3), is kept for exactly that,** as the
   manual way to archive the files left on one machine, and is no longer the normal way to end a day. By then nothing
   may be running to ask whether the day ended, so instead of asking the leading sequencer it takes the operator's
   word, given with `--operator-says-the-day-ended` and recorded, with the operator's name, at the head of the
   listing. **Built.** Its code for listing, checking and moving files, `archive_files` in `scripts/trading_day.py`,
   is the code the launcher is to use.

7. **Files that belong to no single component.** The launcher archives every file of its component, including the
   captured console output, standard output and standard error, which it holds open for as long as the component
   runs and closes once the component has exited. Two files belong to no single component:
   - **the run manifest**, written by the start-of-day procedure for the whole installation on a machine;
   - **the exported `credentials.toml`**, also written by the start-of-day procedure, and read and rewritten during
     the day by both authentication services, which can run on the same machine and use the same file.

   The next day's start moves both into the previous day's archive, once it has found no other file of that day that
   a component reads when it starts, and before it writes new ones. **Built** (section 3, step 2). When another file of
   that day is present, a component missed the end of day, and the start refuses and moves nothing, so that the run
   manifest is still there to name the day to `trading_day.py end` (decision 6). Neither file can be taken for the new
   day's: the run manifest names its trading day, and `credentials.toml` is exported again before any component
   starts.

8. **A component started during the end of day.** A component restarted after the end-of-day end was written finds
   it in its last-state file (decision 5) and does what it would have done had it been running. A component restarted
   between the two events takes part in completing the copies like any other. A component that also meets the
   end-of-day end in records it receives while being caught up has to act on it once, after it has caught up,
   and not part of the way through.

### 4.3 What is built today

`scripts/trading_day.py end` is a procedure run by an operator on a machine, after the venue has entered the end
of day. It does not move the venue on by itself.

1. **It checks that the venue is in the end-of-day end** for the day named in the run manifest, by asking the
   leading sequencer for its state with a `TradingStateQuery` (234), which writes nothing and is answered with a
   `TradingPhaseAnswer` whose outcome is `StateOnly`. If no sequencer leads, or the venue is in an earlier phase,
   it refuses, and says which phase the venue is in and when the end of day is scheduled. In the end-of-day begin
   it refuses too, saying that the end-of-day end follows once the copies of the WAL are complete, because stopping
   the venue before then could leave a copy incomplete. With `--operator-says-the-day-ended` it asks nothing, and
   takes the operator's word instead (section 4.2, decision 6).
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
each copy and its way of moving files across filesystems carry over to the archiving each launcher does, which
uses the same code. The procedure itself is kept as the manual way to archive the files of a component that missed
the end of day (section 4.2, decision 6).

The archive is the day's record. How long archives are kept, and where they are sent, is an operational matter
outside the venue.

### 4.4 Building the rest: items 3, 4 and 6 of BUG-0137

**Status: built, 19 November 2026.** Decisions 9 to 14 below were each agreed with Andrew that day, and every step of the order
below is built and checked by `trading_day_test.py` and scenarios 111, 113, 117 and 118 of `ha_test.py`.

**What every component does on the end-of-day end.** Each is told by a route it already has, or by the one this
section adds, and shuts itself down in an orderly way, writing its last-state file (decision 5) and exiting with
status 3:

| Component | Told by | Before it exits |
|---|---|---|
| Matching engines | the record in the WAL | apply it, close the open-order region |
| Follower sequencer | the record, copied from its leader | hold it in its own WAL and acknowledge it, as for any record |
| Leading sequencer | writing the record itself | wait until the follower sequencer has acknowledged it, or the limit of time has passed (decision 4) |
| Publishers | the record in their copy of the WAL | publish it on `technical_events`, then stay up for the time set in their configuration, 10 seconds |
| Gateways | `OrderAcceptance` from the leading sequencer | log each member out, saying the trading day has ended (decisions 12 and 13) |
| Arbiters, witness, authentication services | the `technical_events` topic, which their thread subscribes to (decisions 10a to 10c) | nothing more |
| Administration service | the `technical_events` topic, from Java (decision 11) | nothing more |

A component that meets the end-of-day end in records it receives while being caught up acts on it once it has caught
up (decision 8).

**Departures from the end on are expected (item 3).** A component that has received the end-of-day end, and is still
running because it is waiting as the table says, treats the loss of any other component as expected: it logs the loss
at Info, saying the trading day has ended, and does nothing else. So the leading sequencer does not step down when its
peer or the arbiters leave and its lease runs out, the follower sequencer does not ask to lead when its leader leaves,
a matching engine does not wait for a sequencer, a gateway does not connect to its sequencer again or tell members
trading is unavailable, and an arbiter makes no statement about a pair whose instances have left. Before the end, a
loss is treated as it is during the day.

**What the framework gives every C++ component (decision 9).**

- `LastStateFile`: the file of decision 5, at the path in the component's configuration; it writes the trading day and
  the phase under a temporary name and renames it into place, and reads it at start-up, so that a component finding
  the end-of-day end of its own day exits at once with status 3.
- A way for a component to stop the reactor for the end of day, after which its `main` returns status 3
  (`STOPPED_FOR_THE_END_OF_DAY`, the status `scripts/launch.py` already acts on).

**Order of building (decision 14).**

1. The framework pieces, with unit tests.
2. The components that already receive every phase: the matching engines, both sequencers, the publishers and the
   gateways, each shutting down as the table says and keeping its last-state file; `ha_test.py`'s check that runs the
   engines on a copy of the WAL gives them a last-state path of their own (decision 5).
3. `TopicSubscriberThread` reworked and `topics.dsl` moved into the framework (decisions 10a, 10b); the arbiters, the
   witness and the authentication services subscribe on their own thread, and shut down (decision 10c).
4. The administration service subscribes, and shuts down.
5. Expected departures in every component.
6. The tests of item 9: `trading_day_test.py` checks a day ending by itself, and scenarios in `ha_test.py` stop the
   leader between the two events, and between writing the end and the follower sequencer acknowledging it.

#### Decision 9. Does the framework provide `LastStateFile`, and a way to stop the reactor for the end of day after which `main` returns status 3, used by every C++ component? Agreed: yes.

#### Decision 10a. Is `TopicSubscriberThread` reworked so that it claims only its own connections, to the two publishers, and the topic's messages, passes everything else to virtual functions the derived thread implements, and follows both publishers, keeping to the one that leads? Agreed: yes.

Today it treats every connection made and every framework message as the topic's, and connects to one publisher, so a
thread that also does other work cannot be built on it.

#### Decision 10b. Does `topics.dsl` move into the framework library, generated in the framework's namespace like the library's other message definitions? Agreed: yes.

The framework's own topic classes use it; it is generated today in the applications' namespace.

#### Decision 10c. Do `ArbiterThread`, the witness's thread and `AuthenticationThread` inherit from `TopicSubscriberThread` instead of `ApplicationThread`, each subscribing to `technical_events`, recognising the end-of-day end of its own trading day from the framework's `TechnicalEvent`, and stopping for the end of day, on the one thread it already has? Agreed: yes.

`TechnicalEvent` and the phase `EndOfDayEnd` are defined in the framework's `leader_follower.dsl`, which the library
generates in its own namespace, so the library's `ArbiterThread` recognises the end of day without help from its
application. Each component is given the publishers' addresses and the topic's port in its configuration, filled in
by the deployment. A thread of its own for the subscription, proposed first, was rejected: one thread does both, and
the subscription is written once, in the framework.

#### Decision 11. Does the administration service subscribe to `technical_events` from Java, with a subscriber of its own that connects to both publishers, keeps to the one that leads, and on the end-of-day end makes the service exit with status 3? Agreed: yes.

The service already speaks framework messages from Java (`PduChannel`), but only one request and one reply at a time
over TLS; the subscriber is new work: the subscription, receiving pages, acknowledging them, and connecting again. The
alternative, asking the leading sequencer for the phase every few seconds with `TradingStateQuery`, is simpler, but
the leading sequencer exits soon after writing the end, and a question asked after that is answered by nobody, so the
service could miss the end; the publishers stay up after publishing it for exactly this. The subscription also gives
the operator's desktop the phase ([BUG-0266](../bug_list.md#bug_0266)).

#### Decision 12. Does the FIX gateway, on the end-of-day end, send every logged-on member a `Logout` (35=5) whose `Text` (58) says the trading day has ended, before it shuts down? Agreed: yes.

A member's connection would otherwise simply close. Members already know the session is closed, from the
`TradingSessionStatus` (35=h) sent at the close; the `Logout` tells them why the connection ends.

#### Decision 13. Is a logout message, carrying its reason as text, added to the binary protocol, and sent by the binary gateway to every logged-on member on the end-of-day end, before it shuts down? Agreed: yes.

The binary protocol has no logout message today: a member's session ends only by the connection closing. Without one,
a binary member is told nothing at the end of day beyond the session's status. With it, the binary gateway does what
the FIX gateway does under decision 12.

#### Decision 14. Is the work built in the order above? Agreed: yes.

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
  of changing it. As the normal way to end a day, the `end` command is to be replaced by each component shutting
  down on the end-of-day end and its launcher archiving its files (section 4); it is kept as the manual way to
  archive the files of a component that missed the end of day.
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
4. **The end of day is two events in the WAL.** The end-of-day begin ends trading, and nothing shuts down on it. The
   end-of-day end is written once the follower sequencer and both publishers hold every record up to the begin, or
   once a limit of time set for each environment has passed, and every component shuts down on it (section 4.2,
   decisions 3 and 4).
5. **Another component leaving is expected only from the end-of-day end onwards.** Between the two events a
   departure is a failure, handled as it would be during the day (section 4.2, decision 2).
6. **There is no order of shutdown.** Each component shuts down as soon as it receives the end-of-day end, except
   that the leading sequencer waits for the follower sequencer to confirm it holds the end, and each publisher stays
   up for a fixed time after publishing it (section 4.2, decision 2).
7. **Each component's launcher archives the component's files** once the component has exited with the status that
   means it stopped for the end of day. `trading_day.py end` is kept as the manual way to archive the files of a
   component that missed the end of day, on the operator's word (section 4.1, and section 4.2, decisions 5 and 6).
