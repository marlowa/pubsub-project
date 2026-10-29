# Phase 6: test events instead of Quill log wording {#test_events}

**Status: agreed** with Andrew on 18 October 2026 (decisions 1 A, 2 A, 3 yes, 4 yes, 5 B, 6 yes, 7 yes). Nothing in
it is built yet.

## 1. What phase 6 does

The high availability tests in `scripts/ha_test.py` decide whether a scenario passes by reading the venue's
Quill logs: they wait for a phrase to appear, count the lines holding a phrase, or read a number out of a line.
That ties every test to the wording of messages written for people, and to the moment Quill's background
thread happens to write them to the file. 67 messages in the C++ carry a `TEST CONTRACT` comment because a test
depends on their exact wording ([BUG-0169](../bug_list.md#bug_0169)). When a test fails, it is often unclear
whether the venue went wrong or the test misread a log.

Phase 6 makes the tests decide from two things instead:

1. **What a member sees.** Wherever a fact can be checked from the messages a member receives (execution
   reports, rejects, resends, logons and logouts), the test checks those messages.
2. **Test events.** For the steps inside the venue that no member can see, such as which sequencer leads or a
   follower being handed over, each component sends the test a small message saying what happened, in a form
   defined in the DSL. A component sends test events only when its configuration names a test to send them
   to, so a venue that members trade on sends none.

Quill logs then go back to being written for people. A message no test reads loses its `TEST CONTRACT`
comment and can be worded for the people who read it.

The phase is finished when no high availability test reads a Quill log to decide whether it passes, the
scenarios pass, and changing the wording of any Quill message cannot make a test pass or fail.

## 2. What the tests read now

The inventory, [test_events_inventory.md](test_events_inventory.md), lists every place the script reads a Quill
log: 171 calls, grouped by the kind of fact they check, each with what it proves and what replaces it. Some calls
are inside helpers used several times, such as `wait_for_me_ord` and `accepted_by_engine`, so a full run makes
more checks than that.

| Kind of fact | Calls | Can a member see it? |
|---|---|---|
| A. Who leads: the role changes of sequencers, matching engines and arbiters | 47 | Not which instance leads. A member sees only whether the venue accepts orders |
| B. What the matching engine did with an order or a cancel | 32 | Sometimes. Not for orders sent with `inject_order`, whose reports go to no member by design; not while a scenario blocks the reports on purpose; and not "applied exactly once", since a repeated report marked as a possible repeat is legitimate |
| C. The matching engine's catch-up completing, or falling short | 2 | No |
| D. Replication, repair and the rule that a follower that is behind does not lead | 14 | No |
| E. Halting, and cancelling open orders on promotion | 10 | The cancels and the refusals, yes. That the engine has halted, no |
| F. Gateway sessions: binding to an instance, disconnection, cancelling on disconnection | 14 | The reports and cancels, yes. Which instance a session is bound to, no |
| G. Resends and session replay | 18 | The resent messages and their sequence numbers, yes. Which sequencer answered or declined, no |
| H. Connections between components made, lost or refused | 10 | No, except the member's own connection |
| I. Throttles and logons | 7 | The reject, logout or logon, yes. The limits the gateway applied, no |
| J. The WAL written in order and undamaged, over the whole run | 2 | Not by a member; but the test can read the WAL files themselves |
| K. The test's own fault-injection library saying it has begun blocking | 5 | Not a Quill log, but the test matches the library's own line on standard error, which phase 6 replaces with a file the library creates (decision 5) |
| L. The helper that reads complete lines, used by the counts above | 4 | Goes when its callers go |
| M. The gateway's progress counts, from a line it logs every few seconds | 4 | Whether the cancellation reports reached the member, yes. That the gateway reports its progress on a clock, and its figures, no |
| N. The FIX test client's own record of what it received | 2 | It is what the member received; not a Quill log of the venue, so it stays |

In all, 125 calls become test events, 33 move to what a member receives, 2 to the WAL files, and 5 to the
fault-injection library's file; 2 stay and 4 go. The events they need are listed at the end of the inventory: 32 as
first named, several of which may merge when they are defined in step 3.

## 3. The design

### 3.1 Checks moved to what a member sees

The test clients already receive what a member receives: `scripts/fix_load_client.py`, which can record the
time of each order's first report (`--answer-times`), `scripts/fix_raw_client.py`, and the binary clients
`binary_client` and `binary_load_client`. Where a client does not yet report what a check needs, it is made to.
For kinds B, E, F, G and I, a check moves to the client wherever the scenario sends its orders through a
gateway: the test asks the client for the reports, rejects, resends or logouts it received, and checks them.
This is the strongest form of check, because it tests what the venue promises its members rather than what it
says about itself.

The scenarios that use `inject_order` hand an order straight to a sequencer, under a session no gateway holds,
to test the matching engine's own refusals, which no gateway would let through. Their reports reach no member,
so those checks use the matching engine's test events (decision 2).

### 3.2 Test events

**What an event is.** A message defined in a new DSL file of the library, `test_events.dsl`, with a name and
the fields a test needs: an instance, an epoch, a record number, a count. The DSL file is the whole contract
between the components and the tests, in place of the wording of 67 scattered messages. Events are named in the
library's terms, not the venue's (for example "a follower was handed over at record N", not "the matching
engine caught up"), since the library is not to name the venue's ideas ([BUG-0161](../bug_list.md#bug_0161)); an
application may define events of its own in its own DSL file.

The list of events is not settled here. It comes out of step 1, from the checks that need one, and is kept to
what those checks need. A first idea of its shape, for the largest kind (A):

- `RoleChanged`: the group (sequencer, matching engine, arbiter), the instance, the role it now has and the
  epoch, sent by the library's lease classes whenever the role changes.

**How events reach the test (decision 1).** Before it starts the venue, the test opens a listening socket on
the loopback interface, on a port the operating system chooses, and passes its address to each component in
its configuration (`[test_events]`). A component so configured opens an ordinary framework connection to it at
start, and sends its events on it as PDUs. This uses what the framework already does well:

- it frames each message, so the test reads whole messages however the bytes arrive;
- sending never makes the sending thread wait: a send is queued and the reactor writes it;
- a connection whose reader does not keep up is closed once 65,536 sends wait on it, so a test that stops
  reading fails visibly instead of slowing the venue down.

A file written through Quill was considered and is not proposed: Quill writes in the background, so the test
cannot know when the file holds every event, which is the weakness this phase removes.

**Order.** Events from one thread reach the test in the order that thread sent them. Events from different
threads, or different processes, have no order between them, and a test must not assume one. Each thread that
sends events therefore has a connection of its own, and the test keeps each connection's events apart.

**Knowing that nothing happened.** A test that checks that something did not happen, such as no second leader
or no order accepted twice, must know when it has seen every event that could have shown it. The test sends a
checkpoint request on a connection; the thread at the other end answers it after everything it did before it
received the request. Since a connection delivers in order, every event that thread sent before the answer has
arrived by the time the answer does. A negative check is made at a checkpoint, never on a timeout.

**Failure.** The test fails, naming the component and the connection, if an event connection closes while the
component's process goes on running, if a message does not decode, if its message id is not one the DSL defines,
or if an event a check waits for does not come. A connection also closes, and nothing is wrong, whenever the test
stops or kills a component, or a supervisor restarts one. So each thread gives its process id when it introduces
itself, and when a connection closes the listener looks at that process: if it has ended, the close is expected;
if it is still running five seconds later, the test fails. The test therefore never has to say which components it
is about to stop. A component whose event connection is closed writes a Warning
to its Quill log and carries on: the test has already failed, and the venue does not stop for a test.

**Cost when switched off.** In a venue with no `[test_events]` section, no connection is made, and each place
that sends an event costs one test of whether the connection exists. Events are sent on role changes,
hand-overs, catch-ups and connections, which happen rarely. The one kind that could be sent for every order is
the matching engine's outcome for an order (kind B), which is why decision 2 keeps it to the orders no member
can see. The order's round trip is measured with the events switched off before and after, as in phases 4
and 5.

**Switched on by configuration, not by the build** (Andrew, 18 October 2026). A build with the events compiled in or
out would mean the tests test a different binary from the one members trade on, and two builds to keep working. So
the rule is: no event is sent from the per-order path, except the matching engine's outcome for an order sent with
`inject_order` (decision 2), and that one only once its cost has been measured. Every step that adds a place that
sends an event measures the order's round trip with the events switched off, before and after, and records the
figures here.

### 3.3 The Python generator

The test harness is written in Python, so it needs to decode the events, and to encode the checkpoint request.
The DSL compiler, which is itself written in Python, has generators for C++, Java and pybind11 bindings. Phase
6 adds a fourth, `python/dsl/generator_python.py`, that writes plain Python: nothing to compile, using only the
standard library (`struct`, `dataclasses`, `enum`). Andrew chose this on 18 October 2026, over decoding with the
pybind11 bindings or decoding by hand.

**What it writes.** One Python module for each DSL file. In it:

- for each enum, an `IntEnum` class with the same entries and values;
- for each message, a dataclass with the fields in the order the DSL gives them, its message id and version as
  class constants, and three functions: `encoded_size()`, `encode()`, which returns the bytes, and `decode()`,
  which takes a buffer and a position and returns the message and the position after it;
- a function `first_invalid_field()`, as the C++ generator writes, which names the first field whose value is
  not allowed: an enum value the DSL does not list, or an empty string in a field that is not optional;
- nothing imported from another generated module: the DSL loader copies the declarations of an included file into
  the file that includes it, so each module stands alone.

**The wire format** is the one in [dsl_design.dox](../dsl_design.dox), section "Wire format": little-endian
integers, a four-byte length before each string, bytes value and list, no length before a fixed array, and a
one-byte flag before an optional field. The Python types are `int` for the integers and `datetime_ns`, `bool`,
`str` for strings (UTF-8), `bytes`, `list` for lists and fixed arrays, `None` for an absent optional field, and
the dataclass for a field that is another message. The DSL's `char` type, which that section does not list,
is one byte, as in the C++ generator.

**Decoding behaves as the C++ decoder does.** It accepts any value for an enum field, as the C++ decoder does,
keeping a value the enum does not list as a plain `int`; `first_invalid_field()` is what rejects it. A buffer
that ends too soon raises `DecodeError`, naming the message and the field, and never returns part of a message.
Encoding a value out of its field's range, such as 200 in an `i8`, raises `EncodeError`.

**The PDU header** is not described by the DSL. Its layout is fixed by `PduHeader.hpp`: 24 bytes, in network
byte order (big-endian), holding the payload's length, the message id, the version, the sequence number and a
canary value that must be `0xC0FFEE00`. The framing is written once by hand, as `python/dsl/pdu_framing.py`,
beside the generator: it reads a header, checks the canary, and returns whole messages from a stream however the
bytes arrive, keeping an incomplete one until the rest comes.

**Python 3.8.** The Rocky Linux 8 image in the project's `Dockerfile` has Python 3.8, and this development
machine has 3.12. The generated code must run on both, so it uses nothing newer than 3.8: no `match`, and
`typing.List` rather than `list[...]` in annotations.

**Where the code goes.** Generated into the build tree, never into the source tree, as the C++ is; installed by
the release step into `installed/lib/python/`, from where `ha_test.py`, which runs against the installed venue,
imports it (decision 3).

**How it is shown to be right.** The Java generator's tests check only the text it generates; they never compile
the Java or compare its bytes with the C++ code's. That is not enough here, so the Python generator is checked
against the C++ code byte for byte:

1. A DSL file written for the test, `every_type.dsl`, has messages using every kind of field: each integer
   size at its smallest and largest value, `bool`, `char`, `datetime_ns`, empty and non-empty strings and bytes,
   strings that are not ASCII, lists of each kind including lists of lists and of messages, fixed arrays,
   optional fields present and absent, enums, and messages inside messages.
2. A small C++ test program, built from the C++ generated for that file, encodes a set of values for each message
   and writes the bytes to files.
3. The Python test decodes each file and compares every field with the values; encodes the same values and
   compares the bytes with the C++ program's, exactly; and hands its own bytes to the C++ program, which decodes
   them and checks every field.
4. The same for the PDU header, against the bytes the framework's own `PduFramer` writes, compiled into the C++
   program.
5. Every DSL file in the project is generated, and the modules are imported, on Python 3.8 in the Rocky image
   and on 3.12 here.
6. Each test is made to fail on purpose before it is trusted: a generator that writes a big-endian integer, that
   leaves out an optional field's flag, that writes a list's count in two bytes, or framing that ignores the
   canary.

The Java generator is checked the same way, with the same `every_type.dsl` and C++ program: the Java generated
for it is compiled with the Java 17 compiler, which this machine and the Rocky Linux 8 image both have, and a small
Java test program decodes the C++ bytes, encodes the same values, and compares, as the Python test does. The Java
clients already show that Java and C++ agree on every message they send and receive (`authentication.dsl` in the
admin service, `fix_orders.dsl` in the FIX test client); this check covers the kinds of field no Java client uses
yet, such as lists of lists and fixed arrays (decision 4).

### 3.4 How each kind of check moves

| Kind | Becomes |
|---|---|
| A. Who leads | `RoleChanged` from the library's `ReplicatedWal`, the matching engine and the arbiter; `LeaseChanged` from the library's `PairLeaseAgent`; `LeaseGrantedTo` from the arbiter; a component's start from its introduction; and a sequencer's connection to a matching engine from `ConnectionChanged` |
| B. The engine's outcome for an order | Execution reports at the client, for orders sent through a gateway; the matching engine's `OrderOutcome` for orders sent with `inject_order`, for the checks no member can make, and where the member has gone (decision 2) |
| C. The engine's catch-up | The matching engine's `CatchUp` event at each stage of catching up: begun, incomplete, completed, and current but waiting to be told what it may do |
| D. Replication and repair | Events from the library's `ReplicatedWal`: running as if alone or not, the WALs agreeing through a record, records discarded, the leader saying its follower may or may not lead; from the arbiter, a statement found on disk; and the sequencer's own events for orders that waited in the WAL and for checking the end of the WAL for repeats |
| E. Halting and cancelling on promotion | The matching engine's events: its book cancelled, with the orders it held and the reports it sent; resumed as leader, with its book; halted, with the reason; and its region found unusable at startup |
| F. Gateway sessions | The FIX gateway's own events: a session announced bound or unbound, a report dropped for want of a session, a member gone with orders open, cancels sent, a session reclaimed; and the sequencer's: a report dropped because its gateway is not connected, a session recorded as unbound, the reports sent when it binds again |
| G. Resends and replay | The resent messages, their numbers and the gap-fills at the client, as before; the FIX gateway's events for the boundaries the client is checked against (the numbering resumed, a resend requested and completed, a new leader asked again); and the sequencers' events for answering or declining a replay |
| H. Connections | `ConnectionChanged` from the channel of the component whose connection it is, including connections made before the test connected; and the matching engine's events for the reports it holds without a connection, releases, or discards |
| I. Throttles and logons | The FIX gateway's events: a provisioned session admitted, a logon refused with the reason, a session established with whether its numbering was confirmed, the throttle limits applied, and throttling started and stopped; the rejects themselves at the client, as before |
| J. The WAL in order and undamaged | The test reads the WAL files after the scenario, which it already knows how to do; no log and no event |
| K. The test's own library | The library creates a second file when it begins blocking, and the test waits for that file instead of a phrase (decision 5) |

## 4. Steps, and how each is known to be right

1. **The inventory, check by check.** For each of the checks, the fact it proves, whether a member could see it,
   and what replaces it, written into the inventory. Andrew reviews it before step 3, because it fixes the list
   of events. Done: [test_events_inventory.md](test_events_inventory.md), for review.
2. **The Python generator and the framing**, built and checked as section 3.3 says, and the Java generator checked
   byte for byte with the same test DSL file and C++ program. This needs nothing from step 1 and can be done
   alongside it.
3. **The channel**, on its own, before any check moves: the library's side (the event connection and answering a
   checkpoint) and the test's side (the listener, decoding, waiting for an event, the checkpoint), with one check
   of each kind moved as the first example, one that something happened and one that something did not. Tested
   with deliberate faults: the test stops reading, a connection is lost, a message arrives split across reads and
   joined with the next. The order's round trip is measured with events switched off, before and after.
4. **The checks moved, one kind at a time,** in the order of the table in section 3.4, largest first. For each
   kind: the replacement is written, made to fail on purpose, and only then is the log check removed and the
   `TEST CONTRACT` comments on its messages taken out. Every scenario passes after each kind.
5. **The proof that no test reads Quill wording.** The helpers that read logs are removed from `ha_test.py`, and
   a run with every `TEST CONTRACT` message reworded on purpose passes.

### 4.1 What step 2 built

**The generator.** `python/dsl/generator_python.py` writes the module section 3.3 describes, and
`python/tools/generate_cpp_from_dsl.py` writes it with `--python`. `python/dsl/pdu_framing.py` reads and writes the
PDU header and returns whole PDUs from a stream. Every DSL file of the project generates a module that imports and
round-trips its messages, on Python 3.12 here and on Python 3.8.17 in the project's Rocky Linux 8 image.

**The check against the C++ code.** `python/tests/resources/every_type.dsl` and `python/tests/cross_language.py`,
with the tests in `python/tests/test_generator_python.py`. For each of 15 sets of values, the Python code writes the
same bytes as the C++ code and decodes the C++ bytes to the same values; for 4 PDUs, `pdu_framing` writes the same
bytes as `PduFramer`. The Java code writes the same bytes as the C++ code for 14 sets of values, and decodes and
encodes the C++ bytes unchanged. Six deliberate faults were each caught: an integer written big-endian, an optional
field's flag left out, a list's count written in two bytes, the rule for an optional field at the end of a message
removed, framing that ignores the canary, and a Java 16-bit integer written in four bytes.

**What it found.**

- The C++ generator cannot write a list of enums, and writes the smallest `i64` value as a literal the compiler warns
  about ([BUG-0177](../bug_list.md#bug_0177)). A list of enums is left out of the test DSL file, and the Python
  generator's handling of one is tested on its own.
- The Java generator did not compile an `i64` enum with a value outside the 32-bit range; fixed, since it stopped the
  Java check running ([BUG-0178](../bug_list.md#bug_0178)).
- The Java code decodes an enum value its enum does not list as `null`, and cannot then encode the message again
  ([BUG-0179](../bug_list.md#bug_0179)). The case holding such values is left out of the Java checks.

The pybind11 bindings were tried as the C++ reference first, and do not serve: they include the C++ generator's code,
so they cannot be built for a list of enums either. The reference is the small C++ program the design called for.

### 4.2 What step 3 built

**The library's side.** `libraries/pubsub_itc_fw/include/pubsub_itc_fw/test_events.dsl` defines the first four
messages: `TestEventsHello`, with which a thread introduces itself; `TestCheckpointRequest` and
`TestCheckpointReached`; and `RoleChanged`. `TestEventChannel` holds one thread's connection to the test, encodes
each event into a buffer of its own and sends it through its owner (`TestEventLinksInterface`, implemented with
`send_pdu_payload`), and answers checkpoint requests. `TestEventsConfigurationLoader` reads the optional
`[test_events]` section. The library's `ReplicatedWal` reports each change of role through the channel it is given
(`report_test_events_to`), and reports its present role when asked (`report_role_to_test_events`), which its owner
does when the connection is made, so that a change made before then is not lost. The build generates a Python module
from every library DSL file and installs them, with `pdu_framing.py`, into `lib/python`.

**The sequencer** is the first component to send test events: with a `[test_events]` section it registers the
service, connects when it is ready, and passes its connections and messages to the channel first.

**The test's side.** `scripts/test_event_listener.py` opens the listener before the venue starts; `ha_test.py`
writes the section into a copy of the sequencer's configuration in `launch_app`, so every start and restart of a
sequencer has it. The listener reads its sockets only while the test waits, with no thread of its own. It fails the
test on a message it cannot decode, a message id the DSL does not define, a framing error, or a component's connection
closing while its process goes on running.

**The first two checks moved.** In every scenario, the wait for the expected sequencer to lead is a wait for its
`RoleChanged` to leader. In scenarios 63 and 64, the check that the follower did not take the lead waits the same
time, asks the follower for a checkpoint, and then finds no `RoleChanged` to leader from it.

**How it was shown to work.** Seven unit tests of `TestEventChannel` and one of `ReplicatedWal`'s reporting; nine
tests of the listener, among them a message sent a byte at a time, three joined in one send, a wrong canary, an
unknown message id, a connection closing, and a checkpoint answered, not answered, and asked of a component with no
connection. Four deliberate faults in the C++ were each caught by its test: a change of role not reported, the present
role not reported, a checkpoint not answered, and a wrong component name. Scenario 63, with its negative check made to
look for any event from the follower, failed at that check. All 76 high availability scenarios pass, from fresh WALs, each waiting for the expected sequencer to lead through
its test events.

**The order's round trip, with no test events configured,** measured as in phase 5, with the secondary matching engine
leading, five runs of 6,000 orders and the first discarded: the medians of four runs were 101.5, 108.3, 108.3 and
108.4 microseconds with the build before this step, 105.8, 108.4, 108.4 and 108.2 with it, and 108.7, 108.4, 108.2 and
108.2 with the build before it again. The step does not change the round trip.

### 4.3 Step 4, kind A: who leads

**The events.** `test_events.dsl` gains three messages, and the introduction gains the process id:

- `LeaseChanged`, sent by the library's `PairLeaseAgent` for every instance that has one (sequencers, matching
  engines and arbiters) when the instance asks the other two voters to let it lead, is granted a lease (saying
  whether the peer's grant or the third voter's made the majority), finds its lease has run out, finds that no voter
  granted it a lease within a lease period, or restarts having been leading and asks to lead again at once;
- `LeaseGrantedTo`, sent by the active arbiter when it grants a lease to a different instance of a pair from the one
  it last granted one to;
- `ConnectionChanged`, sent by `TestEventChannel` itself for each of its thread's other connections made or lost
  after the connection to the test was made.

**The components.** The matching engine and the arbiter now send test events as the sequencer does: each reports its
changes of role, reports the role it already has when the connection to the test is made, and gives its lease agent
the channel. The test writes the name it knows the component by into the `[test_events]` section (`component`), and
the component gives that name in its introduction, because two components can be started from one configuration file.

**What the test waits for.** Each scenario step that waited for a line in a Quill log now waits for events, stated
in the scenario table as an expectation (`Expect`): `expect_leads`, `expect_follows`, `expect_role`,
`expect_started`, `expect_lease`, `expect_granted_to`, and `expect_engine_reachable`, which needs both the engine's
introduction and a sequencer's connection to it, because the kernel completes a connection as soon as the engine
listens, which can be before its thread runs. A step that waits for what follows a kill, a stop or a restart
considers only events that arrive after it. A step that asks what a component has done since it started considers
the events since that component's newest introduction, which is what reading its Quill log from the start did, since
a restart deletes that Quill log. Every check that something did not happen asks the component for a checkpoint
first; a component with no connection open has ended, and everything it sent was read before its connection was
seen to close.

**What stays on the Quill log for now.** The steps that check a connection between components (kind H), and those
that check the matching engine's own states while it becomes current: entering reconciling, waiting to be told what
it may do, and the reports it holds back and then releases or discards (kinds C and H). Their checks that the engine
did not take the lead meanwhile are events.

**How it was shown to work.** Unit tests of each new event and of the `component` key (refused when missing or
empty); a `ReplicatedWal` test that its lease agent reports asking to lead and then the grant, which failed with the
report of the grant removed; and listener tests of introductions, of looking only at a component's present process,
and of the process rule, which failed with the process check made to answer "ended". Against the deployed venue, each
kind of check was made to fail on purpose, and each scenario failed at the step meant to catch it:

| Fault | Scenario | Where it failed |
|---|---|---|
| The arbiter's change of role not reported | 2 | waiting for `arbiter_secondary` to lead after the kill |
| No-leader not reported by the lease agent | 8, 9 | waiting for the report that the group has no leader |
| The arbiter's grant not reported | 15 | waiting for `arbiter_primary` to grant instance 2 of the sequencer pair a lease |
| No connection reported by the channel | 18 | waiting for the restarted matching engine to be reachable |
| The matching engine's taking the lead not reported | 16 | waiting for `matching_engine_secondary` to lead after the kill |
| A check that something did not happen, pointed at an event that did happen | 9 | that check, after its checkpoint |

All 76 high availability scenarios pass. A deliberate fault in the C++ code was put in, built and deployed, and the
build restored before the next; the restored files were compared byte for byte with copies taken first.

**The order's round trip, with no test events configured,** measured as for step 3, with the secondary matching
engine leading, five runs of 6,000 orders and the first discarded: the medians of four runs were 104.7, 108.5, 108.6 and
108.2 microseconds with the build of step 3, 108.2, 108.1, 108.4 and 108.1 with this one, and 108.4, 108.3, 108.2 and
107.9 with the build of step 3 again. Kind A does not change the round trip. None of its events is sent on the order
path: they are sent when a role, a lease or a connection changes.

### 4.4 Step 4, kind B: what the matching engine did with an order

**The member's view.** Every order the FIX test client sends is confirmed by the execution report the client receives.
`send_burst` keeps each client's standard output in a file of its own in the log directory, and `ClientReports` reads
the reports from it as they arrive, taking only messages whose CheckSum line has been written. The test counts the
orders it tells each client to send (`send_orders`); the client numbers its ClOrdIDs in the order it sends them, so the
orders of one step are the next numbers after those already sent, and `await_new_orders` waits for a New report for
each of exactly those ClOrdIDs. Phase 3's baseline orders, the interim orders of Phase 4 and the resend scenario's
tail orders are confirmed in this way. A report sent again after a change of leader is counted once, by the first New
report for its ClOrdID. An order sent into a halt is confirmed refused by the Rejected report whose reason is that the
exchange is closed.

Phase 5's recovery orders are counted rather than named (`await_new_orders_since`): the orders that New reports arrive
for after the recovery orders are sent. Orders sent before them can still be in flight, and the client sends in order,
so naming the recovery orders makes them wait behind thousands of earlier orders; counting confirms what the engine's
own count confirmed before, that a thousand further orders were accepted and reported. It does not confirm the
recovery orders themselves when others were in flight ([BUG-0181](../bug_list.md#bug_0181)).

**The matching engine's view.** Where no member can see the outcome, the engine says what it did:
`applications/matching_engine/matching_engine_test_events.dsl` defines `OrderOutcome` (message id 170), with the comp
id, the ClOrdID, for a cancel the OrigClOrdID, what was done (accepted; refused as a duplicate; refused because the
identifier is longer than the book holds; a cancel done; a cancel refused because the order is unknown or an
identifier too long), whether the engine was catching up, and the sequence number. It is sent through the engine's
`TestEventChannel` with `send_event`, which applications use for events they define themselves; message ids 170 to
229 are theirs, and the listener refuses two event modules that define one id.
Within that range the matching engine has 170 to 179, the sequencer 180 to 189 and 210 to 219, and the FIX gateway
190 to 209.

The engine sends `OrderOutcome` only for the comp ids its `[test_events]` section lists in `report_orders_of`. The
test always lists `INJECT-TEST`, the comp id of `inject_order`, whose orders reach no member. It adds the member's
comp id only in the scenarios whose checks no member can make, or where the member has gone: that an order was applied
exactly once (scenarios 66 and 68), acted on while its reports were held back on purpose (65 and 72), cancelled while
the member was away (70), cancelled or refused after the member's session was dropped (50), or not cancelled when its
gateway died (23). In every other scenario, and in any venue members trade on, no outcome is sent. With no
`[test_events]` section, an order costs one test of whether the channel exists. `TomlConfiguration` gained a list of
strings and `has_key`, so that an optional key written with the wrong type is refused rather than taken as absent.

**What stays as it was.** The check that the engine received orders in ascending sequence number reads a message the
engine no longer writes, so it checks nothing ([BUG-0180](../bug_list.md#bug_0180)); it is left until that is decided.

**How it was shown to work.** Unit tests of the list of strings and `has_key`, of `report_orders_of`, and of the
listener decoding an `OrderOutcome` and refusing two modules that share a message id. Each kind of check was made to
fail on purpose, and each scenario failed at the step meant to catch it:

| Fault | Scenario | Where it failed |
|---|---|---|
| Refusals for an over-long identifier not reported by the engine | 58 | the count of refused orders |
| Accepted orders not reported by the engine | 59 | the order sent to the new leader |
| Cancels done, and cancels of unknown orders, not reported | 50 | waiting for the engine's answer to the gateway's cancels |
| Cancels done not reported | 70 | the cancel made while the member was away |
| One order's New report not seen by the client reader | 1 | the baseline orders |
| The halted refusal looked for with the wrong reject reason | 51 | the order sent into the halt |

Seventy-three of the 76 high availability scenarios pass. Scenarios 14, 27 and 36 fail at Phase 5, because the
member is not told of its recovery orders: after a sequencer restarts and then takes the lead it holds no record of the
member's session, and drops the reports ([BUG-0182](../bug_list.md#bug_0182)). The same failure happens with the
binaries from before this step, so the defect is older than the check; the check that read the matching engine's
Quill log could not see it, because the engine did accept the orders. The three are marked as expected to fail,
naming the bug, so that a run reports them and fails if one of them passes.

The FIX test client is started under `stdbuf -oL`, so that each report reaches its output file when it is printed.
Without it the output is buffered in blocks, and the last reports of a burst can wait in the buffer while the test waits
for them.

**The order's round trip, with no test events configured,** measured as for kind A, with the secondary matching engine
leading, five runs of 6,000 orders and the first discarded: the medians of four runs were 108.5, 108.3, 108.1 and 108.0
microseconds with the build of kind A, 108.4, 108.6, 108.3 and 108.1 with this one, and 108.6, 108.4, 108.3 and 108.1
with the build of kind A again. Kind B adds one test of whether the channel exists to each order the engine accepts,
refuses or cancels, and does not change the round trip.

### 4.5 Step 4, kind C: the matching engine's catch-up

**The event.** `matching_engine_test_events.dsl` gains `CatchUp` (message id 171), sent at each stage of the engine
catching up from the sequencer's WAL: when it begins (entering reconciling); when the sequencer says it has sent
everything and what arrived does not account for it, so the engine asks again; when every record sent has arrived,
once each and in order (R-0101); and, after a catch-up at startup, when the engine is current and waiting to be told
whether it may lead. It carries the record reached, the records applied, the records the sequencer said it sent, the
reports sent to members, and the size of the book. A catch-up happens at a start or a change of leader, not for each
order, so the event is sent whatever comp ids `report_orders_of` lists.

A catch-up can begin before the engine's connection to the test is made. So, as with its role, the engine reports
where it stands when the connection is made: catching up; or caught up, and if caught up at startup and not yet
leading or following, waiting to be told.

**The checks moved.** Scenario 58's check that the restarted engine completed its catch-up and accounted for every
record, and that it did not report its catch-up incomplete. And three steps of scenarios 54 and 55 that had stayed on
the Quill log: the restarted engine asks for a catch-up though it does not lead, and becomes current and waits to be
told. The steps about the reports the engine holds back and then releases or discards stay for kind H.

**How it was shown to work.** A listener test of the event. With the event built to send nothing, scenario 54 failed at
the step waiting for the catch-up to begin, scenario 55 at the step waiting for the engine to be current and waiting,
and scenario 58 at the check that the catch-up completed.

From fresh WALs, 73 of the 76 scenarios pass, and scenarios 14, 27 and 36 fail as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 108.2,
108.0, 108.1 and 107.7 microseconds with the build of kind B, 108.1, 107.9, 107.9 and 107.4 with this one, and 98.7,
107.9, 107.8 and 108.0 with the build of kind B again. Kind C sends nothing on the order path and does not change the
round trip.

### 4.6 Step 4, kind D: replication, repair and the follower that is behind

**The library's events.** `test_events.dsl` gains five messages, sent by the library's `ReplicatedWal` through the
channel its owner gives it, and by the arbiter:

- `RunningAloneChanged` (147): the leader began, or stopped, running as if alone, committing without waiting for a
  follower that is too far behind; with the reason, and how far the follower had acknowledged;
- `WalsAgree` (148): the two WALs were found to agree through a record, said by the leader or by the follower;
- `RecordsDiscarded` (149): a follower discarded records its leader does not hold;
- `PeerMayLeadChanged` (150): the leader now says its follower may not lead, or may lead again;
- `StatementRecorded` (151): an arbiter, starting, found on disk a leader's statement that an instance may not lead.
  The statements are read before the arbiter's connection to the test can be made, so it reports them when the
  connection is made.

**The sequencer's events.** `applications/sequencer/sequencer_test_events.dsl` (message ids 180 to 189) defines four:
the storage for orders waiting for the follower is full (`HeldOrdersFull`); the reading thread sends the matching
engine orders that waited in the WAL (`EngineServedFromWal`); it has sent them all and hands the engine over
(`EngineHandedOver`); and the first command that may already be in the WAL arrived, so the sequencer began reading the
end of the WAL to check such commands (`CheckingLogForRepeats`). None is sent for each order. Two more, at 210 and 211, concern
members' sessions: a gateway told the sequencer that a session established before is still connected to it, and
whether the sequencer was leading when told (`SessionAnnouncedAgain`); and the first report dropped for a session since
it was last connected, because it is connected to no gateway that has told the sequencer so
(`ReportDroppedSessionNotConnected`). Scenarios 14, 27 and 36 wait for the first from each sequencer they restart, and
log how long before taking the lead it was told ([BUG-0182](../bug_list.md#bug_0182)). Two more, at 212 and 213, concern
reports held until the sessions are announced: the first report held for a session the sequencer does not know of,
with how many gateway instances have not announced (`ReportsHeld`); and held reports sent when their session is
announced, or dropped once every instance has announced (`HeldReportsReleased`). Scenario 77 waits for both
([BUG-0190](../bug_list.md#bug_0190)). The listener decodes the
sequencer's messages as it does the matching engine's, and `expect_message` in `ha_test.py` waits for any message by
name, with the field values it must have.

**The checks moved.** The fourteen of the inventory's kind D, in scenarios 60 to 64, 67, 74 and 75. A check that read a
component's Quill log from its start, after a restart, looks at the events since that component's newest introduction.

**How it was shown to work.** A `ReplicatedWal` unit test that the leader reports the WALs agreeing, running as if
alone and then not, and saying its follower may not lead and then may lead again, in that order.

Each event was then made to fail on purpose. A scenario stops at its first failure, so the faults were put in four
builds: the first sending none of the events, the next two sending all but the ones each scenario checks second and
third, and the last leaving out only the arbiter's statement. Each scenario failed at the check meant to catch it:

| Fault | Scenario | Where it failed |
|---|---|---|
| No WAL, sequencer or arbiter event sent | 60 | the leader running as if alone |
| | 61 | the leader finding the follower's WAL agrees |
| | 62 | the restarted old leader finding its WAL agrees |
| | 63 | the leader saying its follower may lead |
| | 67 | the leader beginning to check the end of the WAL for repeats |
| | 75 | the storage for waiting orders filling |
| Running as if alone ending not reported | 60 | the leader waiting for the follower again |
| The follower's agreement not reported | 61 | the restarted follower finding its WAL agrees |
| Saying the follower may not lead not reported | 63 | the leader saying its follower may not lead |
| Serving the engine from the WAL not reported | 75 | the reading thread sending the orders that waited |
| Discarded records not reported | 62 | the old leader discarding the records the new leader lacks |
| The hand-over not reported | 75 | the engine handed over |
| The arbiter's statement not reported | 64 | the restarted arbiter holding the statement that instance 2 may not lead |

Scenario 63 does not restart the arbiter, so it passed with the arbiter's statement left out, as it should; scenario 64
is the one that does.

From fresh WALs, 73 of the 76 scenarios pass, and scenarios 14, 27 and 36 fail as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 108.1,
107.9, 107.7 and 107.4 microseconds with the build of kind C, 99.1, 98.5, 107.5 and 107.4 with this one, and 98.5, 97.8,
107.7 and 107.3 with the build of kind C again. The runs near 98 microseconds come with both builds; none of kind D's
events is sent for each order, and it does not change the round trip.

### 4.7 Step 4, kind E: halting, and cancelling on promotion

**The events.** `matching_engine_test_events.dsl` has five messages for these:

- `BookCancelled` (172): the engine cancelled every order on its book, with the number it held when it began and the
  number of cancel reports it sent;
- `ResumedAsLeader` (173): a promoted engine finished catching up and serves again, with the orders on its book;
- `Halted` (174): the engine halted trading, with the reason: no engine could trade for longer than the venue allows,
  so a `CancelBook` command cancelled its book and halted; or its region could not be used and the WAL does not reach
  the first command of the day, so a `TechnicalEvent` halted trading, cancelling nothing;
- `RegionUnusable` (175): at startup, the region holding the open orders could not be used, with the reason;
- `RegionRebuilt` (169): the region could not be used, and the engine rebuilt its book from the WAL, which reached the
  first command of the day, and carries on with it, with the orders it holds.

The region is found unusable, and trading may be halted, before the engine's connection to the test is made, so both
are reported when it is made, with the engine's catch-up.

**The checks moved.** What a promoted engine inherited, cancelled and resumed with, in every scenario with a pair of
engines that changes leader; the cancels after a gateway's death and a long absence (scenario 18); the cancels and the
halt after a long absence (51); and the refused region, the book rebuilt from the WAL, and the absence of any cancel
or halt (52). These were the engine's own book, which no member can see: what it inherited
and what it resumed with. Where the check is that nothing was cancelled, the engine is asked for a checkpoint first.
The three helpers that read these counts from the engine's Quill log are gone.

**How it was shown to work.** With the cancelled book and the unusable region not reported, scenario 16 failed at what
the promoted engine inherited, 18 and 51 at the cancels, and 52 at the refused region. With the halt and the resumption
not reported, 16 failed at what the engine resumed with, 51 at the halt, and 52 at the engine saying it cannot account
for what it held.

From fresh WALs, 73 of the 76 scenarios pass, and scenarios 14, 27 and 36 fail as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 108.1,
107.5, 107.5 and 107.3 microseconds with the build of kind D, 98.9, 98.8, 107.7 and 107.8 with this one, and 107.9,
107.8, 107.5 and 107.2 with the build of kind D again. Runs near 98 microseconds have come with the builds both before
and after a change in earlier measurements (sections 4.5 and 4.6); none of kind E's events is sent for each order.

### 4.8 Step 4, kind F: gateway sessions

**The gateway's channel.** The FIX gateway now sends test events as the sequencer, the matching engine and the arbiter
do: with a `[test_events]` section it registers the service, connects when it is ready, and passes its connections and
messages to the channel first. `ha_test.py` starts every FIX gateway with the section.

**The gateway's events.** `applications/fix_order_gateway/fix_order_gateway_test_events.dsl` (message ids 190 to 199)
defines five: a member's session announced to the sequencers as bound or unbound (`SessionAnnounced`); a report dropped
because it carries no member connection (`ReportDroppedNoSession`); a member gone with orders open, whether they are
cancelled at once, held for a grace period, or left resting, with the grace period (`SessionDisconnectedWithOrders`);
the cancels sent when grace periods end (`CancelsSent`); and a member reconnected within its grace period, its orders
left resting (`SessionReclaimed`). None is sent for each order.

**The sequencer's events.** `sequencer_test_events.dsl` gains three: a report dropped because the gateway instance it is
for is not connected (`ReportDroppedGatewayNotConnected`); a session recorded as unbound (`SessionUnboundRecorded`);
and the reports produced while a member was away, sent when it binds again (`UndeliveredReportsSent`).

**The checks moved.** The fourteen of kind F, in scenarios 18, 19, 21, 22, 23, 50, 70 and every scenario with a pair of
engines that changes leader. The grace period scenario 19 checks is taken from the event's `grace_seconds`, and the
helper that read it from the gateway's Quill log is gone. A check that something did not happen, such as no session
unbound after a resend, counts the events once the component has answered a checkpoint (`count_seen`).

**How it was shown to work.** With no gateway event sent, and neither the sequencer's dropped report nor its unbound
session, scenario 21 failed at the new session's announcement, 18 at the report dropped for the dead instance, 19 at the
orders held for the grace period, 50 at the dropped session's open orders, and 70 at the session recorded as unbound.
With the reclaimed session and the reports sent on binding again not reported, 19 failed at the reclaim and 70 at the
report sent on reconnecting.

The first run of scenario 50 found that the gateway also cancels a member's orders at once, when the member logs out or
its grace period is zero, on a third path the event had not been added to; it was added there.

From fresh WALs, 73 of the 76 scenarios pass, and scenarios 14, 27 and 36 fail as expected under
[BUG-0182](../bug_list.md#bug_0182). One run of the developer loop failed at an integration test of pausing a
connection, which then failed once in five runs on its own; it is recorded as [BUG-0184](../bug_list.md#bug_0184).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 107.8,
107.7, 107.8 and 107.4 microseconds with the build of kind E, 108.4, 107.8, 108.1 and 107.7 with this one, and 108.0,
108.0, 108.1 and 109.0 with the build of kind E again. None of the gateway's or sequencer's new events is sent for each
order, and kind F does not change the round trip.

### 4.9 Step 4, kind G: resends and session replay

**What was already at the member.** The checks of what the member was resent were already made on the FIX test
client's own output: which numbers came back, which order each carries, which reports were marked as possible
repeats, and the gap-fill the member received. What the test still read from the gateway's Quill log were the
boundaries those checks are made against: the number the session resumed at after a reconnect, the bounds the gateway
read off the member's ResendRequest, the number it resumes at after the resend, how many reports it resent, what it
gap-filled, and how many of the session's numbers it was told had held reports.

**The events.** The FIX gateway's DSL file gains four: the session's numbering resumed, with how many of its numbers
held reports (`SessionResumed`); a ResendRequest read, with its bounds and the number the session resumes at
(`ResendRequested`); the resend completed, with the reports resent and the gap-fill (`ResendCompleted`); and a new
sequencer leader asked again for a resend that was waiting (`ReplayAskedAgain`). The sequencer's DSL file gains two: a
replay answered, and a replay declined because the instance does not lead (`ReplayAnswered`, `ReplayDeclined`).

**The checks moved.** The eighteen of kind G, in scenarios 22, 23, 40 and 73. That exactly one sequencer answered and the
other declined is counted once both have answered a checkpoint, after waiting for the decline, which comes on the
gateway's request to the other sequencer. The four helpers that read these boundaries from the gateway's Quill log are
gone.

**How it was shown to work.** The events of one scenario are checked one after another, so the faults were put in five
builds, each scenario failing at the check meant to catch it:

| Fault | Scenarios | Where they failed |
|---|---|---|
| The numbering resumed and the request read not reported | 22, 23 | the session's numbering resumed |
| | 40 | the gateway reading both bounds off the request |
| The request read not reported | 22, 23 | the member's request for the gap |
| The resend's completion not reported | 22, 23, 40 | the gateway finishing the resend |
| A replay answered not reported | 40 | exactly one sequencer answering |
| Asking the new leader again not reported | 73 | the gateway asking the new leader again |

From fresh WALs, 72 of the 76 scenarios passed, scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182), and scenario 59 failed: the old leader's WAL did not hold the last of three orders
sent to it just before it was stopped. The scenario stops the leader the moment the orders are sent, which can beat the
leader to reading the last one; scenario 62 had the same race and waits for the WAL instead. It is recorded as
[BUG-0185](../bug_list.md#bug_0185), and scenario 59 passed in each of three runs on its own with the same build.

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 101.0,
108.5, 108.1 and 108.2 microseconds with the build of kind F, 101.8, 108.3, 108.1 and 107.8 with this one, and 99.0,
107.7, 107.8 and 107.7 with the build of kind F again. None of kind G's events is sent for each order, and it does not
change the round trip.

### 4.10 Step 4, kind H: connections made, lost and refused

**Connections made before the test.** `TestEventChannel` reports each of its thread's other connections made or lost
(`ConnectionChanged`). It now also remembers the connections made before its own connection to the test, forgets any
of them that are lost, and reports the rest as made when the test connects. Without that, a connection a component
accepts in its first moments, such as a matching engine's book replication connection, could be made before the test
was connected and never be reported, and a test waiting for it would fail at random. `ConnectionChanged` also carries
the reason a lost connection gives.

**The binary gateway.** The binary gateway now sends test events as the FIX gateway does, and `ha_test.py` starts it with
a `[test_events]` section. It needs no events of its own yet: its connection to the sequencer is a `ConnectionChanged`.

**The matching engine's events.** `matching_engine_test_events.dsl` gains three: the engine has no connection to a
sequencer for reports, so it acts on orders and holds their reports (`ReportsHeld`); it sends the reports it held, because
a connection was made or because it now serves (`ReportsReleased`); and it discards the reports it held, because
another instance served those members (`ReportsDiscarded`).

**The checks moved.** The ten of kind H, and the steps of scenarios 54 and 55 about the reports the engine held. The
matching engine's book replication connection is the connection the engine accepts on its replication listener, whose
port is read from the engine's deployed configuration when the check runs (`expect_replication_connection`). In scenario
71, the gateway's connection to the sequencer closing while the sequencer is stopped, and so still holds its socket
open, can only be the gateway closing it; the check is that it closed, without reading the reason. The check that the
gateway did not close the member's own connection counts the connections lost on the gateway's member listener up to
the moment the test closes that connection itself, after a checkpoint.

**How it was shown to work.** The faults were put in six builds of the venue, each scenario failing at the check meant
to catch it:

| Fault | Scenarios | Where they failed |
|---|---|---|
| A lost connection not reported | 71 | the gateway closing its connection to the stopped sequencer |
| The engine's held reports discarded not reported | 54 | the reports from the engine's catch-up discarded rather than sent |
| The engine's held reports released not reported | 55 | the held reports released now that the engine serves |
| | 72 | the engine sending the reports it held once its connections were let through |
| The engine holding reports not reported | 72 | the engine saying it was holding reports |
| The binary gateway making no connection to the test | 57 | the binary gateway connecting to the primary sequencer |

The connections made before the test connected are not reported in a seventh build, and scenarios 29, 32 and 37 passed
with it. In that run, each connection they wait for was made after the test had connected, which is the usual order; the
remembered connections are reported only when it is the other way round, which is too rare and too dependent on timing
for a scenario to catch on purpose. The unit test `AConnectionMadeBeforeTheTestIsReportedWhenTheTestConnects` failed
with the same fault, and passed again without it. The check in scenario 71 that the gateway did not close the member's
own connection reads for an event that should never come, so it was shown to see one in a copy of `ha_test.py` whose
window runs on past the test closing that connection itself, and which reads every event sent up to then: the check
failed, on the gateway reporting the member's connection lost when the test closed it.

From fresh WALs, 73 of the 76 scenarios passed, and scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 99.9,
108.0, 108.2 and 107.9 microseconds with the build of kind G, 100.1, 99.4, 99.4 and 99.0 with this one, and 108.0, 107.4,
107.4 and 107.5 with the build of kind G again. The round trip settles at about 100 or about 108 microseconds from one run
to the next with any build, as the first run of each kind G measurement shows, so this build's lower medians are not a
gain. Kind H adds nothing to the path of an order, and it does not slow the round trip.

### 4.11 Step 4, kind I: throttles and logons

**The gateway's events.** `fix_order_gateway_test_events.dsl` gains six, at message ids 200 to 205, because the
gateway's first ten ids are used and no message definition in the project uses the ids 200 to 299. Applications' test
events now have the ids 170 to 229, and the FIX gateway has 190 to 209. The six are: a member provisioned for particular
gateway instances admitted at one of them, with its primary and backup instances and the instance that admitted it
(`SessionProvisioned`); a logon refused, with the reason (authentication failed, the authentication service not proved
genuine, provisioned for other instances, or throttle limits out of range) and, when provisioned elsewhere, the
instances it is provisioned for (`LogonRefused`); a session established, with whether the sequencer reported the
session's numbering before the gateway opened it (`SessionEstablished`); a member's limits per second on new orders,
amends and cancels applied to its session (`ThrottleApplied`); the first refusal of a run for one kind of command, with
the limit (`ThrottleStarted`); and the end of that run, with how many were refused and whether a command was accepted
again or the session ended (`ThrottleStopped`).

**The checks moved.** The seven of kind I, in scenarios 20 and 56, and the wait for each FIX test client's logon that
every scenario makes, which read the gateway's Quill log for the outcome and is listed among the reads the first sweep
missed. The wait now takes the first `SessionEstablished` or `LogonRefused` from the gateway after the client starts. A
session opened without its numbering confirmed is still a failure there, named as itself. The line the gateway logs
when a session is established keeps its `TEST CONTRACT` comment, because `perf_run.py` and `callgrind_run.py` still
match it.

**How it was shown to work.** The faults were put in four builds of the gateway, each scenario failing at the check
meant to catch it:

| Fault | Scenario | Where it failed |
|---|---|---|
| A provisioned session admitted not reported | 20 | the gateway admitting the comp id as provisioned for instances 1 and 2 |
| The throttle limits applied not reported | 56 | the gateway applying the limits of 5 new orders, 2 amends and 3 cancels a second |
| A logon refused at the wrong instance not reported | 20 | the gateway refusing the comp id provisioned for instance 2 |
| Throttling started not reported | 56 | throttling of new orders started exactly once |
| A session reported established alongside the refusal | 20 | no session established at the wrong instance |
| Throttling stopped not reported | 56 | throttling of new orders stopped exactly once |
| Every session reported as opened without its numbering confirmed | 1 | the first logon of the scenario, named as opened without confirmed numbering |

The fault that reports a session established alongside the refusal shows that the check that no session was established
reads the events that would show one.

From fresh WALs, 73 of the 76 scenarios passed, and scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 108.1,
107.9, 107.5 and 107.7 microseconds with the build of kind H, 99.0, 98.7, 107.7 and 107.7 with this one, and 107.8,
107.6, 107.7 and 107.8 with the build of kind H again. The runs settle at about 100 or about 108 microseconds with any
build, as section 4.10 says. None of kind I's events is sent for each order, and it does not change the round trip.

### 4.12 Step 4, kind J: the WAL in order and undamaged

**The check moved.** After every scenario, `ha_test.py` reads both sequencers' WALs whole, as they stand on disk, and
fails the scenario if either holds a record out of order or is damaged. A record is out of order when its number is not
one more than the number of the record before it, whether it repeats a number or leaves a gap; the first record may have
any number. An entry is damaged when it is not whole or its checksum is wrong, and a valid entry follows it, later in
its segment or in a later segment. The unfinished entry a process leaves at the end of the WAL when it is killed while
writing is not damage, because nothing follows it. These are the rules the library's `WalReader` and `ReplicatedWal`
apply. The check no longer reads the two messages the sequencers log for these conditions, and the `TEST CONTRACT`
comment on the one in `ReplicatedWal` is gone; the other message, in `WalReader` and `WalCursor`, carried none.

Reading a WAL of 1.7 million records takes about four seconds. A full run's WALs grow as it goes, to about 670,000
records each by the end, and the full run took 36 minutes 28 seconds, against 35 minutes 48 seconds before the check
moved.

**How it was shown to work.** The WALs kept from earlier runs that were found damaged were read with the new check.
Two of them fail it, each for the reason it was kept: in one, the entry at byte 47206 of segment 130 is not valid and a
valid entry follows it, and three records are out of order; in the other, record 402005 is written twice, one directly
after the other. Then scenario 1 was run twice with a copy of `ha_test.py` that reads one of those WALs in place of the
secondary sequencer's: it failed each time, naming the damage in the first and the repeated record in the second.

From fresh WALs, 73 of the 76 scenarios passed, and scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182). The check ran after each of the 73.

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 99.5,
98.9, 107.5 and 108.0 microseconds with the build of kind I, 98.8, 107.8, 107.7 and 107.9 with this one, and 107.9,
108.0, 107.8 and 107.7 with the build of kind I again. Kind J changes no code on the path of an order, and it does not
change the round trip.

### 4.13 Step 4, kind K: the fault-injection library confirms that it is blocking

**The library.** `libblock_sends_to_ports.so` takes a third environment variable, `PUBSUB_TEST_BLOCK_CONFIRMATION`, the
path of a file it creates when blocking begins (decision 5, B). It notices the flag file at the first send after the
flag appears, so the confirmation file appears then too. It still writes its line to standard error at the same moment,
for a person reading the process's output; no test reads that line.

**The checks moved.** The five of kind K, in scenarios 59, 62, 65, 66 and 76. `ha_test.py` gives the library the path
`block_sends_to_ports.blocking` beside the flag file, removes it before each scenario with the flag, and waits for it to
exist (`await_blocking`) where it waited for the line.

**How it was shown to work.** In a build of the library that never creates the confirmation file, scenario 59, where
the primary sequencer's sends are blocked, and scenario 66, where the secondary's are, each failed at the wait for it,
saying that the library never confirmed it had begun blocking.

From fresh WALs, 73 of the 76 scenarios passed, and scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 99.4,
107.9, 107.8 and 107.5 microseconds with the build of kind J, 108.0, 108.2, 107.8 and 108.0 with this one, and 108.4,
107.6, 107.5 and 107.6 with the build of kind J again. The library is loaded only by a test, and kind K does not change
the round trip.

### 4.14 Step 4, kind M: the gateway's progress counts

**The event.** `fix_order_gateway_test_events.dsl` gains `ProgressReported` (message id 206), sent each time the FIX
gateway logs its progress line, every five seconds and after a set number of orders, with the line's figures: orders
accounted for, reports sent and dropped, orders received, orders awaiting an answer, and orders and cancels refused. It is
not sent for each order. The line keeps its `TEST CONTRACT` comment, because `perf_run.py` still reads it.

**The checks moved.** The four of kind M. Two of them are about the gateway's own reporting, which no member sees, so they
read the event rather than the member's counts: in scenario 18, that gateway b, which no member used, reported progress
and every report it made was all zeros; in scenario 42, that the gateway went on reporting while nothing progressed, at
least twice in the time watched, with orders refused and orders awaiting an answer. The third, in scenario 51, is whether
the member was told that its orders were cancelled, and it now counts the cancellation reports the member received, one
for each order the matching engine cancelled. The helpers that read the progress line from the gateway's Quill log are
gone.

**How it was shown to work.** The faults were put in four builds of the gateway, each scenario failing at the check
meant to catch it:

| Fault | Scenarios | Where they failed |
|---|---|---|
| No progress reported | 18 | gateway b reporting no progress at all |
| | 42 | no progress report in the 12 seconds watched |
| Every progress report claiming one order accounted for | 18 | gateway b reporting traffic it should never have seen |
| The refused orders always reported as 0 | 42 | the progress report saying none were refused after orders were refused |
| Cancellation reports not passed on to members | 51 | the member sent none of the 1,000 cancellation reports |

The second shows that the check on gateway b reads figures that would show traffic.

From fresh WALs, 73 of the 76 scenarios passed, and scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 108.0,
108.1, 108.2 and 108.0 microseconds with the build of kind K, 107.9, 107.7, 107.7 and 107.4 with this one, and 107.9,
107.6, 107.6 and 107.4 with the build of kind K again. `ProgressReported` is sent with the progress line, not for each
order, and kind M does not change the round trip.

### 4.15 Step 4: scenario 69, and kind N

**Scenario 69.** Its check of how many kept reports the new leader forwarded was among the reads the first sweep missed.
`sequencer_test_events.dsl` gains `KeptReportsForwarded` (message id 189), sent when the sequencer takes the lead, with
how many of the reports it kept while not leading it discarded because the old leader said it had forwarded them, how
many it forwarded, and their size. The check reads it, and the `TEST CONTRACT` comment on the line the sequencer logs at
the same moment is gone.

**Kind N stays.** Scenarios 22 and 23 read the sequence number of the Logon the member received from the FIX test
client's own output. That is the member's record of what it was sent, not a Quill log of the venue.

**How it was shown to work.** In a build of the sequencer that does not send `KeptReportsForwarded`, scenario 69 failed
because the new leader did not say how many kept reports it forwarded. In one that adds 100 to the count it sends, the
scenario failed because the new leader said it forwarded 102, more than the 10 the check allows; it had forwarded 2.

From fresh WALs, 73 of the 76 scenarios passed, and scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182).

**The order's round trip, with no test events configured,** measured as for kind A: the medians of four runs were 107.9,
107.8, 107.6 and 107.6 microseconds with the build of kind M, 99.5, 99.4, 107.7 and 107.7 with this one, and 107.9,
108.0, 107.9 and 107.6 with the build of kind M again. The runs settle at about 100 or about 108 microseconds with any
build, as section 4.10 says. The event is sent once, when a sequencer takes the lead, and it does not change the round
trip.

### 4.16 Step 4: the unit tests that read a captured Quill log

**The checks moved** (decision 6). `PairLeaseAgentTest` gives each agent a test event channel whose connection records
what it is sent, as an owner started by a test does, and checks the lease changes the agent reports: granted at epoch 1,
the lease ran out, no leader (once from each instance), and resuming the lead after a restart. That a promise which
cannot be recorded is not made is checked by the recorder's own count of failed writes and by the follower granting no
lease. That the follower holds its leader's statement that it may not lead was already checked by the statement it
recorded, so that line went.

`ReplicatedWalTest` checks, through the same kind of channel, the leader running as if alone and stopping, what it says of
its follower, the records a rejoining follower discards, and the WALs agreeing before a catch-up. A request that arrives
while records are being sent to the follower is held until the catch-up stops; the test checks that the catch-up was told
to stop. A helper six of its tests use, which steps until the leader says its follower may lead, read the line saying so;
it now reads `ReplicatedWal::says_follower_may_lead()`, which is new.

**One new library event.** A leader whose application gives it a reading thread sent no event when that thread had sent
a rejoining follower every record it lacked, although a leader that sends them itself says that the WALs agree.
`test_events.dsl` gains `FollowerHandedOver` (message id 152), sent then, with the last record sent.

The `TEST CONTRACT` comments on the messages these tests read, three in `PairLeaseAgent` and five in `ReplicatedWal`, are
gone; nothing reads those messages now.

**How it was shown to work.** Each of these, put in the library one at a time, failed the tests meant to catch it: the
hand-over not reported (the two tests of a catch-up through the reading thread); the records discarded not reported (the
test of a rejoining follower that discards records); running as if alone not reported (the test of a follower that stops
acknowledging, and the test of the events themselves); the lease agent's changes not reported (four of
`PairLeaseAgentTest`, and the test of the agent's events in `ReplicatedWalTest`); the running catch-up not told to stop
(the test of a request from a new position); and `says_follower_may_lead()` always false (the five tests that step until
it is true).

From fresh WALs, 73 of the 76 scenarios passed, and scenarios 14, 27 and 36 failed as expected under
[BUG-0182](../bug_list.md#bug_0182). The order's round trip, with no test events configured, measured as for kind A: the
medians of four runs were 108.6, 108.2, 107.8 and 107.5 microseconds with the build before this change, 107.9, 107.8,
107.8 and 107.5 with this one, and 108.6, 109.1, 109.5 and 108.0 with the build before it again. `FollowerHandedOver` is
sent once for each follower that rejoins, and the change does not alter the round trip.

### 4.17 Step 5: no high availability test reads Quill log wording

**What was removed.** The helpers in `ha_test.py` that read a Quill log (`poll_log_for`, `read_complete_lines`,
`count_log_marker` and `file_end`), the fields of the scenario steps that named a Quill log and the phrases to look for in
it, and the code that read them. Every step gives the test events it waits for, or must not see, and a step that gave
none now fails the scenario at once. The variables left holding a Quill log's name or position, which nothing read, are
gone. So is `check_me_seq_monotonic`, which read the matching engine's Quill log for a line the engine no longer writes and
so always passed ([BUG-0180](../bug_list.md#bug_0180), closed by Andrew's decision): the check of the WALs after every
scenario (section 4.12) covers what it was meant to check.

**What still reads a Quill log, and why.** Scenarios 44, 45 and 47 read the messages an operator is given, by decision 8;
those three messages carry `TEST CONTRACT` comments naming the scenarios. `perf_run.py` and `callgrind_run.py` are not high availability tests: they read the FIX gateway's line saying
a session is established, and both gateways' progress lines and the line each writes when it drops a report for a member
that has gone, and those lines carry `TEST CONTRACT` comments naming them.

**The proof.** Every message marked `TEST CONTRACT` except the three of decision 8, 27 in all, was reworded on purpose:
a `~` was put after every character of its text outside the `{}` placeholders, so that no phrase of two characters or
more survived, whether or not it had a space in it. Changing only the spaces is not enough, because a phrase with no
space in it, such as `ha_enabled=false`, survives it. As a control, the three of decision 8 were reworded too, and
scenarios 47 and 44 then failed, because neither sequencer said why it was leading and the arbiter exited without saying
why. With the 27 reworded and those three as they are, every unit and integration test passed, the installed sequencer was
confirmed to carry the reworded text, and from fresh WALs 73 of the 76 scenarios passed, scenarios 14, 27 and 36 failing
as expected under [BUG-0182](../bug_list.md#bug_0182). The wording was then restored.

**The comments.** No test reads 25 of the 27 any more, and their `TEST CONTRACT` comments are gone. The FIX gateway's line
saying a session is established and its progress line keep theirs, now naming both `perf_run.py` and `callgrind_run.py`.
Three lines those scripts read had no comment, and now have one: the binary gateway's progress line, and the line each
gateway writes when it drops a report for a member that has gone. Eight `TEST CONTRACT` comments remain, each naming what
reads its message.

## 5. What phase 6 does not do

- **No behaviour of the venue changes.** Only what the components tell a test, when one asks.
- **Prometheus metrics are not used for test results.** Prometheus is optional, so nothing the venue or its
  tests need may depend on it (principle V13).
- **Unit and integration tests that do not read log wording are not changed.** Some do read it:
  `PairLeaseAgentTest` checks 6 phrases in the log it captures and `ReplicatedWalTest` 11 (decision 6).
- **Copilot's proposal**, which Andrew supplied, is the starting point; where this design departs from it, it says
  why (sections 3.2 and 3.3).

## 6. Decisions

### Decision 1. How do events reach the test? (Choose A or B.)

- **A.** A framework connection from each component to a listener the test opens on the loopback interface,
  carrying DSL messages (section 3.2). The test knows when it has every event, through the checkpoint, and a
  test that stops reading fails visibly.
- **B.** A file per component, written through a Quill logger of its own. Simpler to build, but the test cannot
  know when the file is complete.

Recommended: A. **Decided: A** (Andrew, 18 October 2026).

### Decision 2. How are the matching engine's outcomes for orders checked? (Choose A or B.)

- **A.** Through the member's execution reports wherever the order went through a gateway, and through matching
  engine events only for orders sent with `inject_order`, whose reports reach no member.
- **B.** Through matching engine events for every order. One mechanism, but an event for each order on the
  engine's thread, and checks that no longer test what members receive.

Recommended: A. **Decided: A** (Andrew, 18 October 2026).

### Decision 3. Where does the generated Python go? (Yes or no.)

Generated into the build tree and installed by the release step into `installed/lib/python/`, from where
`ha_test.py` imports it. Recommended: yes. **Decided: yes** (Andrew, 18 October 2026).

### Decision 4. Is the Java generator checked byte for byte against the C++ code in this phase? (Yes or no.)

The test DSL file and C++ program of section 3.3 serve it too, at a small extra cost. Recommended: no, since the
high availability tests do not use Java. **Decided: yes** (Andrew, 18 October 2026).

### Decision 5. How does the fault-injection library confirm that it has begun blocking? (Choose A or B.)

`libblock_sends_to_ports.so`, loaded into one sequencer, discards everything that sequencer sends on chosen ports
once a flag file the test creates exists. The test must know the block has begun before it goes on. Today the
library writes one line to standard error when it begins, the test captures each component's standard error in a
file, and five checks wait for the phrase there.

- **A.** Keep the line and the five checks that match its wording.
- **B.** When it begins blocking, the library also creates a second file whose path the test gives it, and the test
  waits for that file to exist. No test matches text; the line stays for people reading the output.

Recommended: B. **Decided: B** (Andrew, 18 October 2026).

### Decision 6. Are the unit tests that read log wording in this phase too? (Yes or no.)

`PairLeaseAgentTest` and `ReplicatedWalTest` check 17 phrases between them in the logs they capture, and some of
the `TEST CONTRACT` comments in `PairLeaseAgent` and `ReplicatedWal` are there for them. Unless they change too,
rewording those messages would still break a test, and the phase's last step could not pass. They would check the
class's state, or the events it sends, instead. Recommended: yes, added to the inventory in step 1. **Decided: yes** (Andrew, 18 October 2026).

### Decision 7. Is the design as a whole right to build, in the order of section 4? (Yes or no.)

Recommended: yes. **Decided: yes** (Andrew, 18 October 2026).

### Decision 8. Do the checks of messages written for an operator keep reading the message? (Choose A, B or C.)

Scenarios 44 and 45 start the arbiter and the witness by hand with high availability off, and check that each refuses to
start and that its message says "refusing to start" and "[ha] enabled = false". Scenario 47 starts two sequencers with
high availability off, and checks that each wrote "ha_enabled=false" in its Quill log as the reason it leads. What these
check is that an operator is told, so the wording is what is under test. The arbiter and the witness refuse before they
have any connection to a test.

- **A.** All three keep reading the message, as stated exceptions: their `TEST CONTRACT` comments stay, and step 5 leaves
  those three messages alone.
- **B.** All three are replaced: scenarios 44 and 45 check only that the process exits with a failure, and scenario 47
  checks a sequencer event. Nothing then tests that an operator is told.
- **C.** Scenarios 44 and 45 as in A, and scenario 47 as in B.

Recommended: A. **Decided: A** (Andrew, 20 October 2026).
