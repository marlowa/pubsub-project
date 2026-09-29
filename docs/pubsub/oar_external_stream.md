# The order activity recorder's external stream {#oar_external_stream}

**Status: design, not implemented.** Several decisions are still open. They are listed together in
[section 14](#oar_external_stream_open), and the sections that depend on them say so.

---

## 1. What this document covers

The order activity recorder (OAR) subscribes to the topics of the matching engine publisher and
publishes an event for each outcome of an order to a system outside the venue. That system is called the
external messaging system in this document: Apache Pulsar or Apache Kafka. "External" distinguishes
it from the venue's own pub/sub, which works quite differently. Other systems learn what happened to orders at the
venue by consuming what OAR publishes there.

This document describes how OAR publishes to that system. It covers:

- the interface OAR publishes through, which does not depend on whether the system is Pulsar or
  Kafka, and the library that holds it;
- how the published messages are described, using Avro schemas generated from the DSL;
- how those schemas change over time without breaking the programs that read them;
- how schemas are registered, and what OAR checks when it starts;
- how the published events are kept in order;
- how programs that use the external messaging system are configured, and how the system itself
  is set up;
- that only the leader of OAR's two instances publishes, and where OAR resumes whenever it starts;
- what happens when publishing stops or falls behind, how a failure to publish is handled, how the
  trading day ends, and what operators can see.

Related documents:

- The requirements OAR must meet are R-0049 to R-0053 in the functional specification
  (`docs/book`, the order activity recorder subsection of the high availability chapter).
- [oar_bus_deduplication.md](oar_bus_deduplication.md) records the evidence on whether the outside
  system can be relied on to discard duplicate messages. It concludes that it cannot, and this
  document follows that conclusion.
- Section 7 of [mep_oar.md](mep_oar.md) says briefly where OAR sits beside the matching engine
  publisher, and points here for everything else.

### 1.1 What OAR guarantees, and what it does not

**OAR guarantees:**

- **No event is lost.** Every execution report that produces an event is published and confirmed at
  least once (R-0049). If OAR cannot publish, the backlog grows until trading halts (R-0053), so the
  venue never runs on without its record (sections 2 and 13).
- **Events keep the venue's order.** On a topic, each event's first appearance comes in the order
  the venue sequenced the records (section 9).
- **Every event carries an identity.** The pair of producer identity and sequence number identifies
  each event (R-0050). A repeated event carries the same pair, and is identical to the original byte
  for byte (section 11).
- **Repeats are cheap to recognise.** A consumer needs to remember only the highest sequence number
  it has processed for each producer identity (section 11).
- **Every event can be decoded.** Its writer's schema can always be found from the event itself
  (section 8). Every version of the schema is compatible with every earlier one in both directions
  (section 6).
- **The trading day is fully recorded before it ends.** The end of day is not complete until OAR
  reports that every event up to the `eod` technical event is confirmed (section 13.6).

**OAR does not guarantee:**

- **That an event appears only once.** A restart of OAR, a change of leader or a failed publish all
  publish some events again. Consumers discard the repeats (section 11).
- **Consecutive sequence numbers.** The venue numbers other records from the same sequence, and
  rejected orders publish no event, so there are gaps (sections 4.4 and 9).
- **How quickly an event is published.** OAR publishes as soon as it can. How far behind it may fall
  is limited only by the backlog bound, after which trading halts (section 13).
- **An event for every execution report.** Rejected orders are not published. Fills are not
  published yet, because the venue produces none (section 4.4).
- **How long events are kept.** That is set by whoever runs the external messaging system (section
  10.5).

---

## 2. The rule every part of this design keeps

OAR moves its position, the sequence number of the last record it has finished with, past a record
**only after the external messaging system has confirmed that it has stored the record's event,
and never before.** Where that position is kept, and how OAR finds it whenever it starts, is
described in section 12.1.

The reason is the difference between the two ways a crash can go wrong:

- If OAR crashes after publishing an event but before its position has moved past it, it publishes
  that event again when it restarts. The event appears twice downstream. A consumer can recognise the second
  copy and discard it, because every event carries an identity (R-0050).
- If OAR moved its position first and then crashed before the event was stored, the event would
  never be published. Nothing downstream could detect the omission, because nothing downstream
  knows the event existed. R-0049 forbids this.

Three consequences follow, and each shapes something later in this document.

1. **Publishing is asynchronous and confirmation is reported by sequence number.** OAR hands an
   event to the interface and carries on. Later, the interface reports that everything up to a
   given sequence number has been stored.

2. **OAR tells the matching engine publisher it has consumed a record only once that record's event
   is confirmed.** The publisher uses these acknowledgements to decide which records it may discard
   from its log. `TopicSubscriberChannel` currently sends an acknowledgement after every eight
   records it has delivered to the application. For OAR, delivery to the application only means the
   event has been handed to the interface, not that it has been stored. So the channel needs a
   mode in which the application states how far it has been confirmed, and the channel acknowledges
   no further than that.

3. **A failed publish is never skipped.** If the external messaging system reports that an event could not be
   stored, OAR stops publishing, discards anything it has handed over but not had confirmed, and
   resumes from the last confirmed sequence number. Everything after that point is published again.
   The duplicates this causes are expected and are handled downstream. Section 13.5 describes each
   kind of failure and what OAR does with it.

---

## 3. The external messaging library

The interface and its implementations live in a library of their own, separate from the framework
library `pubsub_itc_fw`. It is called `external_messaging`. The
library knows nothing about orders, topics of the matching engine publisher, or OAR. It publishes
bytes, with the information needed to publish them correctly.

### 3.1 What the library contains

| Part | What it is |
|------|-----------|
| `ExternalMessagingProducerInterface` | The abstract interface OAR publishes through. |
| The test implementation | Records what it is given and confirms when the test says so. Always built. |
| The Pulsar implementation | Publishes with the Apache Pulsar C++ client. Built only when a CMake option enables it. |
| The Kafka implementation | Publishes with librdkafka. Built only when a CMake option enables it. |

Which implementation OAR uses is chosen at run time from its configuration, from among those
compiled into the binary. A build without the Pulsar or Kafka client libraries still builds and
tests everything else.

### 3.2 What a publish carries

Each publish carries five things:

- **The topic.** Which topic in the external messaging system the event goes to, given as a value
  from the generated list of declared topics (section 5.1), not as a string. A mistyped topic is
  then a compile error, not a run-time surprise.
- **The key.** The external messaging system uses the key to decide where the event goes and to keep events
  with the same key in order. For OAR the key is the instrument (the FIX `Symbol`). Section 9
  explains why.
- **The payload.** The event encoded as plain Avro binary, with nothing added in front of it.
- **A schema reference.** The name of the schema the payload was encoded with and its version
  number, for example `OrderEvent` version 3. Each implementation turns this into whatever its
  external messaging system needs, as described in section 8.
- **The sequence number.** The sequence number the venue assigned to the record the event came from.
  Confirmations are reported in terms of it.

The **producer identity** is not carried with each publish. It is given to the implementation once,
from configuration, when it is created. It must be the same on OAR's primary and secondary
instances, and must not be derived from a host name, a process identifier or an instance's role. A
consumer uses the pair of producer identity and sequence number to recognise a duplicate, and a
promoted secondary must present the same identity the primary did. The identity comes from one value
in the environment file, `oar_producer_identity`, which the deploy step writes into both instances'
configurations, so the two can differ only if a deployed configuration is edited by hand.

If a single record from the matching engine publisher ever produces events on more than one topic,
that record counts as confirmed only when all of its events are confirmed, and OAR's position
must not pass it before then. OAR's own tracking of confirmations enforces this: it counts the events
each record produced, and moves its position past a record only when all of them are
confirmed.

Inside the Pulsar implementation there is one Pulsar producer for each topic, because a Pulsar
producer is bound to a single topic. A Kafka producer can send to any number of topics.

### 3.3 Confirmations and threads

OAR's state belongs to OAR's own thread and must not be touched from any other thread. So an
implementation never passes a confirmation to OAR directly. It places the confirmation on OAR's
inter-thread queue, and OAR handles it on its own thread, like any other message. OAR sees
confirmations and failures as ordinary events on its own thread and never learns that another thread
was involved.

The two client libraries report confirmations differently, and each implementation does whatever
its library requires to reach that queue:

- **The Pulsar C++ client** runs threads of its own and reports each confirmation by calling a
  function on one of them. That function places the confirmation on OAR's queue.
- **librdkafka** reports a confirmation, which it calls a delivery report, only from
  `rd_kafka_poll()`, on whichever thread calls it. The Kafka implementation therefore has a helper
  `ApplicationThread` of its own, which calls `rd_kafka_poll()` and places each confirmation on
  OAR's queue. Polling could instead be done on OAR's own thread, which the project prefers to
  adding a thread. The helper thread is used so that confirmations reach OAR by the same path with
  either system, and OAR's own code does not depend on which is in use.

**How the helper thread knows there is something to collect.** librdkafka can write to a file
descriptor whenever a confirmation arrives in an empty queue (`rd_kafka_queue_io_event_enable`). A
small `EventHandler`, registered with the reactor through `Reactor::register_handler`, watches that
descriptor. When it fires, the handler only sends the helper thread a message; librdkafka's
functions never run on the reactor's own thread. The helper thread then calls `rd_kafka_poll()` with
a timeout of zero, repeatedly, until it reports that there was nothing to collect, and returns. So a
confirmation is collected as soon as it arrives, with no polling interval, and no call ever waits
inside librdkafka, where the thread could not handle its own queue or the framework's termination
event. A slow recurring timer, about every 100 milliseconds, runs the same loop as a safety net, in
case a wake-up is ever missed; when there is nothing waiting, its first poll returns at once.
Whether `register_handler` is safe to call from application code at the point the Kafka
implementation calls it is to be checked when it is built.

The test implementation reports its confirmations the same way, from a thread of its own created
with `ThreadWithJoinTimeout`. This is deliberate. It means the ThreadSanitizer build exercises the
hand-over from a foreign thread to OAR's thread, which is the part of this code most likely to be
wrong, even though the Pulsar and Kafka implementations are not built under ThreadSanitizer.

### 3.4 What the test implementation can do

A test can make the test implementation:

- hold confirmations until the test releases them, so that the test can crash and restart OAR at a
  chosen point between a publish and its confirmation;
- report a failure for a chosen event, to check that OAR resumes from the last confirmed position
  and publishes everything after it again;
- report everything it was given, in order, so that the test can check that nothing is missing and
  that anything published twice is an exact duplicate.

---

## 4. How the published messages are described

### 4.1 Why Avro

Events are encoded in Apache Avro. Avro is the format most commonly used with both Pulsar and Kafka.
Every major programming language has a library that reads it, the tools built around Pulsar and
Kafka understand it, and it has published rules for changing a schema without breaking programs
that read older or newer versions.

The venue's own DSL binary format is not used on the external messaging system, for three reasons:

- Events stay in the external messaging system for as long as it is configured to retain them. A consumer may
  read an event months after it was written, using a program built against a later version of the
  schema. The DSL has no rules for that situation, and inside the venue it does not need them,
  because both ends of every connection are built together.
- The tools built around Pulsar and Kafka cannot read the DSL's format.
- The DSL generates Java and C++ code, but a consumer written in any other language would need
  another generator.

The DSL is still where `OrderEvent` is declared. The Avro schema is generated from it (section 5).

### 4.2 Why Avro needs the writer's schema

Avro's binary encoding does not describe itself. A record with a long field holding 5 and a string
field holding "VOD" is written as five bytes: `0A 06 56 4F 44`. `0A` is 5 in Avro's variable-length
integer encoding, `06` is the string's length (3) in the same encoding, and the remaining three
bytes are the letters. There are no field names, no markers between fields, and nothing saying how
many fields there are.

So a program can decode a record correctly only if it knows the exact schema the record was written
with. Avro calls this the **writer's schema**. The schema the reading program was built against is
the **reader's schema**. Avro's specification defines **schema resolution**: given both schemas,
the Avro library matches fields by name. It supplies the reader's default for a field the writer
did not write, and reads and discards a field the reader does not know. With resolution, a program
built against one version of a schema can read records written with any compatible version.

Every consumer of OAR's events must therefore be able to find out which schema each event was
written with. How it does so differs between Pulsar and Kafka, and is described in section 8.

### 4.3 The event: `OrderEvent`

Every event OAR publishes is an `OrderEvent`. It is a single record type able to describe every
kind of order event. Messaging systems call a record used this way an envelope.

It is a single type because all events for an order must be published to one topic, so that they
stay in order (section 9), and one record type is the only way to carry several kinds of event on
one topic that both Pulsar and Kafka check fully.

- **Kafka.** The registries used with Kafka decide which subject a schema belongs to by a subject
  name strategy. The default, `TopicNameStrategy`, gives each topic one subject, so every record on
  the topic must conform to one schema's history. The alternatives, `RecordNameStrategy` and
  `TopicRecordNameStrategy`, name the subject after the record type, so that one topic can carry
  several record types, each with its own history and its own compatibility check. Every consumer
  must then be configured with the same strategy.
- **Pulsar.** Pulsar keeps one list of schema versions for each topic, and checks each new version
  against the topic's compatibility strategy. Carrying several unrelated record types on one topic
  therefore appears to require that topic's strategy to be set to always compatible, which switches
  the check off. This has not yet been confirmed against a running Pulsar.

### 4.4 What an event records: outcomes

OAR exists so that systems outside the venue can maintain order books and show live orders. Each
event therefore records an **outcome** that changes the set of live orders: something that happened
to an order at the venue. It does not record a request a member made. The venue tells the member of every outcome with an execution
report, so OAR's events are derived from execution reports, and OAR takes its input from the
matching engine publisher's `execution_reports` topic. OAR also follows the technical events, which
tell it when the trading day ends (section 13.6); they produce no events of their own.

Recording outcomes rather than requests matters in two cases:

- A cancellation request can be refused, for example when the order has already gone. A record of
  requests would show the order as cancelled when it was not.
- The venue cancels some orders itself, and those cancellations appear only as execution reports,
  never as a request on the `orders` topic. A record of requests would miss them.

The kinds of event, and the execution reports they come from:

| Event kind | Execution report's `ExecType` | Meaning |
|------------|-------------------------------|---------|
| `Added` | `New` | The venue accepted the order and it is now open. |
| `Amended` | `Replaced` | The venue changed an open order. The venue cannot yet amend an order, so no event of this kind is published yet. |
| `Cancelled` | `Canceled` | The order is no longer open because it was cancelled, whether at the member's request or by the venue itself. |

The matching engine currently produces execution reports of three kinds: `New`, `Canceled` and
`Rejected`. It produces no fills. `Rejected` reports are not published. A rejected order was never
added, so it never appears in an order book or among live orders, and it is of no interest to the
systems OAR serves.

**The kind of event.** `event_kind` is an Avro enum with the symbols `Unknown`, `Added`, `Amended`
and `Cancelled`, and a default symbol of `Unknown`. The default matters for schema evolution. When
a program built against an older version of the schema meets a symbol it does not know, the Avro
library gives it the default instead of failing. A kind of event added in a later version therefore
reaches older programs as `Unknown`, and they can skip it.

`Amended` is in the schema from its first version, although the venue cannot yet amend an order.
It costs nothing now, and it means amendment needs no change to the kinds of event when the venue
supports it.

### 4.5 The fields of an event

The fields follow the pattern of the execution report the event comes from. Fields that exist only
for fills are left out, because the venue produces no fills. If it comes to produce them, they are
added as optional fields with defaults, which is a compatible change (section 6).

**Fields every event has:**

| Field | Avro type | Contents |
|-------|-----------|----------|
| `seq_no` | `long` | The sequence number the venue assigned to the execution report. |
| `producer_id` | `string` | OAR's producer identity (section 3.2). |
| `event_kind` | enum | Which kind of event this is (section 4.4). |
| `exec_id` | `string` | The venue's identifier for the execution report (FIX `ExecID`). |
| `order_id` | `string` | The venue's identifier for the order (FIX `OrderID`). |
| `symbol` | `string` | The instrument. |
| `side` | enum | Buy or sell. |
| `leaves_qty` | `string` | The quantity still open after this event (section 4.6). |
| `cum_qty` | `string` | The quantity executed so far. Always 0 while the venue produces no fills. |
| `transact_time_ns` | `long` | The venue's time for the event, in nanoseconds since the Unix epoch (FIX `TransactTime`). |
| `venue_time_ns` | `long` | When the venue recorded the execution report, in nanoseconds since the Unix epoch. |

An event records no time of its own publication. Every field of an event comes from the execution
report, so an event made again from the same report, as happens whenever OAR publishes again, is
identical to the first one byte for byte (section 11). The time an event reached the external
messaging system is recorded by the system itself, as a timestamp on every message; a consumer that
wants to know how long publishing took compares that timestamp with `venue_time_ns`.

**Fields that are present when the execution report carries them.** Each is declared as either
null or a value, with a default of null:

| Field | Avro type | Contents |
|-------|-----------|----------|
| `cl_ord_id` | `string` | The member's identifier for the order (FIX `ClOrdID`). |
| `orig_cl_ord_id` | `string` | For `Amended` and `Cancelled`, the member's identifier of the order as it was before (FIX `OrigClOrdID`). |
| `ord_type` | enum | The order type. |
| `price` | `string` | The limit price (section 4.6). |
| `stop_px` | `string` | The stop price. |
| `order_qty` | `string` | The order's total quantity. |
| `time_in_force` | enum | How long the order stays open. |
| `expire_time_ns` | `long` | When the order expires, in nanoseconds since the Unix epoch. |
| `account` | `string` | The member's account, if one was given. |
| `min_qty`, `max_floor` | `string` | The minimum and displayed quantities, if given. |
| `parties` | array of records | The parties to the order, as in the execution report's parties group. |
| `text` | `string` | Free text the execution report carries. |

How the member is identified is still to be confirmed
against what consumers of such a stream need (section 14, question 2); until then the design follows the
execution report, which identifies the member through `account` and `parties`.

### 4.6 Prices and quantities

Prices and quantities are published as Avro `string` values, holding the decimal text exactly as the
venue carries it, for example `101.25` or `1500`. The venue carries them this way from end to end:
FIX writes them as decimal text, and the DSL generator maps the FIX types `PRICE`, `QTY`, `AMT` and
`FLOAT` to strings. OAR copies the text into the event unchanged, so no conversion can fail and no
value can be altered.

The venue never needs a price or a quantity as a number. It does no arithmetic with them and does
not store them in a database; it only passes them on. Text is therefore sufficient, and it is also
safer than any numeric type, because none both represents every such value exactly and has no
limit on its size:

- **Binary floating point** cannot represent most decimal fractions exactly. 0.1, for example, has
  no exact binary representation.
- **An integer holding the value multiplied by a fixed factor** is exact but limited. With a 64-bit
  integer and a factor of 1,000,000, the largest whole amount that fits is 9,223,372,036,854, which
  is thirteen digits. Amounts in currencies such as the yen can have sixteen or more significant
  figures.
- **Avro's `decimal` type** fixes a precision and scale in the schema, which imposes the same kind
  of limit, and changing it later is not a compatible change.

The text is in FIX's decimal format: an optional sign, digits, and at most one decimal point, with
no exponent and no limit on the number of digits. The FIX order gateway checks every decimal field
against this format, although the check itself has a defect ([BUG-0095](../bug_list.md#bug_0095)).
The binary order gateway does not check it at all ([BUG-0096](../bug_list.md#bug_0096)). Until both
are fixed, a malformed value can reach the published events.

**OAR does not go into service until both are fixed.** OAR itself cannot keep a malformed value out
of the stream: it may not drop an event (R-0049), and refusing to publish one would stop publishing
and, after the backlog bound, trading (section 13). The only place a malformed value can be refused
is where it enters the venue, in the gateways.

**What this asks of consumers.** A consumer that needs to calculate or compare with a price parses
the text into an exact decimal type of its own, such as Java's `BigDecimal` or Python's
`decimal.Decimal`. Prices must be compared by value, never as text: `101.25` and `101.250` are the
same price, and the venue passes on whichever form the member sent.

### 4.7 Why one record with optional fields

**Why not a choice between several record types.** Avro also
offers a union: a field that holds one of several record types, with a small index in the encoding
saying which. A union of an "added" record, an "amended" record and a "cancelled" record looks
tidy. But adding a fourth kind of event later means adding a fourth choice to the union, and a
program built before that change cannot decode an event that uses the new choice. The full
compatibility rule in section 6 would therefore refuse that change. With one record, a new kind of
event is a new enum symbol and some new optional fields, and both changes are compatible in both
directions.

---

## 5. Declaring topics and generating the Avro schema from the DSL

### 5.1 Which topics exist

Which topics exist in the external messaging system, and which message type each one carries, is part
of the design, not configuration. It changes only through a release, like the schemas. The topics
are declared in the DSL, in the same way `applications/pubsub.dsl` declares the venue's own topics
and their member messages:

```
external_topic order_events {
    OrderEvent
}
```

The generator produces the list of declared topics and their message types for the C++ code, for
the deploy step and for the release artefact. Consumers therefore receive, with each release, the
list of topics that exist and the schema each one carries.

**Each topic carries exactly one message type.** Where several kinds of event must stay in order
relative to each other, they share a topic, and that topic's message type is an envelope such as
`OrderEvent`. One message type for each topic works identically on Pulsar and Kafka, keeps each
topic's full compatibility check switched on in both, and uses the Kafka registries' default subject
naming, which most consumers and tools assume. Section 4.3 explains why several record types on one
topic would not. The rule also makes the ordering promise explicit: events on different
topics have no guaranteed order relative to each other, so which topic an event goes to is a
reviewed design decision.

What each declared topic is called in a particular deployment is configuration, described in
section 10.

### 5.2 Generating the schema and the encoder

`OrderEvent` is declared in a DSL file that belongs to OAR, with a version number in the usual
`version=N` form. A new output of the DSL generator, written in Python alongside the existing Java
output in `python/dsl/generator_java.py`, produces two things from it:

- the Avro schema, as a `.avsc` file;
- a C++ encoder that writes an `OrderEvent` in Avro's binary encoding.

The encoder is generated rather than taken from the Avro C++ library. Avro's binary encoding is
simple and fully specified: variable-length integers, length-prefixed strings and bytes, and an
index for each union or enum value. Writing it directly avoids adding the Avro C++ library and its
build requirements to the venue.

**The encoder does not need to keep up with changes to Avro, because the encoding does not change.**
Avro's binary encoding has been the same since Avro 1.0. What later versions added, such as logical
types for dates, timestamps and decimals, are annotations on the existing types: a timestamp is still
written as a `long`. A generated encoder written today writes what every Avro library, old or new,
reads. That the generator might one day have to follow a change to the encoding is a concern in
theory only; it would take a new major version of the Avro specification, which every Avro library
would have to follow too.

Those build requirements are about to grow. The Avro C++ library's next major version replaces the
fmt library with the standard library's `std::format` and requires C++20
([AVRO-4260](https://issues.apache.org/jira/browse/AVRO-4260)). gcc 8.5 on RHEL8 cannot compile
that: the standard library first has `std::format` in GCC 13. A venue that depended on the Avro C++
library would be held to Avro 1.12 on RHEL8.

**The encoder is checked against Avro's own library.** A test encodes events with the generated
encoder, decodes them with the Avro C++ library using the generated `.avsc` file, and checks that
every field comes back as written, for every kind of event and with every optional field both
present and absent. This is the direct way to catch a mistake in the generator. It makes the Avro C++
library a dependency of that test only, never of the venue; `scripts/build_avro_cpp.sh` builds it
into the third-party directory.

DSL types map to Avro types as follows:

| DSL | Avro |
|-----|------|
| `i8`, `i16`, `i32` | `int` |
| `i64` | `long` |
| `bool` | `boolean` |
| `string` | `string` |
| `bytes` | `bytes` |
| `datetime_ns` | `long`, holding nanoseconds since the Unix epoch |
| `optional T` | a union of `null` and T, with a default of null |
| an enum | an Avro `enum`, which the generator requires to name a default symbol |
| `list<T>` | an Avro `array` of T |

Prices and quantities are strings in the venue's messages, because FIX represents them as decimal
text. `OrderEvent` carries them as text too (section 4.6).

---

## 6. How the schema changes over time

### 6.1 The rules for a change

- Add a field only at the end of the record.
- Never remove a field.
- Never change the meaning of a field.
- Give every added field a default, so that a program reading an older event, which lacks the
  field, has a value to use.
- Increase the version number with every change.

A program that resolves schemas matches fields by name, so for such a program the position of a
field does not matter. Adding fields only at the end still costs nothing, and it protects programs
that decode without resolving.

**Nothing is reserved in advance for a future use.** Fills are the obvious example: the venue
produces none yet, and when it does, events will need fields to describe them. Those fields are
added then, as optional fields with defaults. Under these rules that is a compatible change in both
directions, so it needs no preparation now and cannot be rushed later: a new release adds the
fields, older consumers ignore them, and newer consumers reading older events get the defaults.
Reserving the fields now would do harm. Their names and types would be fixed before fills are
designed, and because a field is never removed, a reservation that turned out wrong would stay in
the schema for good.

### 6.2 The compatibility rule: full transitive

Avro, Pulsar and the Kafka schema registries share a vocabulary for checking a new version of a
schema against earlier ones:

- **Backward:** a program built against the new version can read events written with the old ones.
- **Forward:** a program built against an old version can read events written with the new one.
- **Full:** both.

Each rule comes in two forms. The plain form checks the new version only against the version
before it. The transitive form checks it against every earlier version.

OAR's schema uses **full transitive**. The programs that consume OAR's events belong to other
people and are upgraded on their own schedules, so old programs read new events. The outside
system retains events and consumers replay them, so new programs read old events written under many
versions. Both directions matter, against every version. The rule must be in place before the first
version is registered, because tightening it later fails against versions already registered.

### 6.3 Released schema files

Every build generates the Avro schema for the version the DSL currently declares, into the build
tree. In addition, the repository holds one file for each version that has been released, for
example:

```
schemas/released/OrderEvent/v1.avsc
schemas/released/OrderEvent/v2.avsc
```

A released file is never edited.

The build enforces three rules:

1. **If the DSL's version has already been released**, the generated schema must be identical to
   its released file. A change to the DSL without an increase in the version number fails the
   build.
2. **If the DSL's version has not been released yet**, it must be one more than the highest released
   version, and the generated schema must pass the full transitive check against every released
   file.
3. **Making a release** copies the generated schema into `schemas/released/`, in the release
   commit. From then on rule 1 applies to that version.

The check itself uses the compatibility checker in Avro's Java library, run from a small Maven
module in `java/`. No registry is involved: an incompatible change fails on the developer's machine
before any registry or consumer sees it.

Released files are kept in the repository, not regenerated from old release tags, for two reasons.
A build from a source tarball has no history to regenerate from. And the generator's output layout
may change over time, whereas the text of a released schema is what was registered and must never
change.

### 6.4 What a release contains

The release artefact contains every file in `schemas/released/` and the DSL file that declares
`OrderEvent`. Consumers need the `.avsc` files, which every Avro library reads. They are also what a
registry can be rebuilt from.

---

## 7. Who may change the schema

OAR is the only producer of `OrderEvent`, and the schema changes only through a release of this
project. A consumer that needs a new field asks for it through the project; it then arrives in a
release, having passed the checks in section 6.

The registry enforces this. Only the deploy step has permission to register versions of OAR's
schema. Consumers are given read-only access, so no consumer, and no developer outside the project,
can register a version.

---

## 8. Registration, and what OAR checks when it starts

A **schema registry** stores schema texts and identifies each one, so that a consumer can find the
writer's schema of any event it reads. **Registration** means storing a schema in the registry.
The registry checks each new version against the compatibility rule and refuses a version that
breaks it.

OAR never registers anything. The deploy step registers every released version, in order, before
OAR is started.

Pulsar and Kafka do this differently, so the two are described separately.

### 8.1 With Pulsar

**The registry is part of the broker.** Pulsar keeps the schema versions of each topic itself. There
is no separate registry process to install, run or back up. If OAR can reach the broker, it can
reach the registry.

**The version travels in the message's metadata.** The broker records the schema version in a
metadata field of each message. The payload is plain Avro with nothing in front of it.

**The deploy step** sets these policies on the namespace OAR publishes to:

| Policy | Setting | Why |
|--------|---------|-----|
| Schema compatibility strategy | `FULL_TRANSITIVE` | Section 6.2. Pulsar's default when nothing is set is `FULL`, which checks only against the previous version. |
| `isAllowAutoUpdateSchema` | false | With it true, which is the default, any producer that connects with a new compatible schema registers it. Registration must be done by the deploy step only. |
| `schemaValidationEnforced` | true | With it false, which is the default, a producer that sends raw bytes with no schema may publish to a topic that has one, and nothing checks what it wrote. |
| `brokerDeleteInactiveTopicsEnabled` | false for this namespace | With it true, which is the default, the broker deletes a topic it considers inactive, and the topic's schema with it. |

It then uploads each released version in order with `pulsar-admin schemas upload`.

**Minimum broker version.** Enforcing schema validation broke geo-replication until the fix in
Pulsar pull request 25012. The fix is in Pulsar 4.2.0, 4.1.3, 4.0.9 and 3.0.16. A deployment that
uses geo-replication needs a broker at or above the fixed release on whichever line it runs.

**When OAR starts**, it creates its producer with the schema it was built with, declared as an Avro
schema. The producer's name is the producer identity, and each message's sequence identifier is the
venue's sequence number. The broker recognises the schema as the registered version. If that
version was never uploaded, creating the producer fails, and OAR refuses to start and logs an
error.

### 8.2 With Kafka

**The registry is a separate process.** Apache Kafka has no schema registry. The registries in use
implement an HTTP interface with JSON bodies that Confluent defined for its own registry. Two
registries under the Apache 2.0 licence implement it: Apicurio Registry and Karapace. Which one is
open (section 14, question 1).

**The registry's numbers.** The registry groups the versions of one schema under a name it calls a
subject. Because each topic carries one message type (section 5.1), each topic has one subject,
named after the topic's full name with `-value` added, for example `prod.venue.order_events-value`.
This is the registries' default naming. Each registered version is given a number that is unique
across the whole registry. That number is what identifies the writer's schema of an event.

**The number travels at the front of the payload.** Each payload starts with five bytes: a byte of
0, then the registry's number as a big-endian signed 32-bit integer. This is the arrangement the
registries' serializers use, and every Java deserializer and Kafka tool that works with a registry
understands it.

The sequence number and the producer identity travel in Kafka record headers, separately from the
schema.

**The deploy step**, for each message type OAR writes:

1. Sets the subject's compatibility rule to `FULL_TRANSITIVE`.
2. Registers each released version in order. Registering a version that is already registered
   returns its existing number and changes nothing, so running the deploy step again is harmless. If
   the registry refuses a version as incompatible, the deploy stops with an error.
3. Checks that the registry's version number for the last one equals the version the DSL declares.
   If it does not, something else has been registered under that subject, and the deploy stops.
4. Writes the result into OAR's configuration:

```toml
[[external_messaging.kafka.schemas]]
message = "OrderEvent"
version = 3
registry_id = 17
```

An OAR binary writes exactly one version of each message type: the version compiled into it. So its
configuration holds one entry for each message type it writes. It never needs the numbers of older
versions. A consumer reading an older event finds that event's number in the event itself.

**When OAR starts:**

1. **It checks its configuration against itself, without using the network.** For each message
   type compiled into OAR, the configuration's entry must exist and must name the version compiled
   in. If it does not, the configuration came from a different release's deploy. OAR refuses to
   start and logs an error.
2. **It checks with the registry, if the registry can be reached.** It fetches the schema for each
   configured number and compares it with the schema compiled in. If they differ, OAR refuses to
   start and logs an error. Publishing would label every event with the wrong schema, and every
   consumer would misread every event, which is worse than not publishing. If the registry cannot
   be reached within a few seconds, OAR logs a warning and continues, because check 1 has already
   shown that the configuration came from the matching deploy.
3. **It publishes.** Every payload starts with a byte of 0 and the four bytes of the configured
   number.

OAR contacts the registry only in check 2, so a registry that is down when OAR restarts does not
stop OAR publishing. This matters because trading halts when OAR cannot publish (R-0053).

**The producer must be idempotent.** librdkafka's `enable.idempotence` setting defaults to false.
Without it, a send that fails and is retried can land after later sends, and events are then out of
order. The Kafka implementation sets it to true.

Idempotence here keeps events in order and stops a retried send being stored twice **while one
producer is running**. It does not prevent duplicates across a restart of OAR or a change of leader:
the new producer is a different producer to Kafka, and it publishes again from its resume position.
Those duplicates are expected, and consumers discard them (section 11).

**Kafka transactions are not used.** Transactions are the Kafka feature for writing to several
places atomically, and are often suggested for removing duplicates. They are not used, for three
reasons:

- **They would not remove OAR's duplicates by themselves.** A transaction removes duplicates only
  when the producer's read position is stored in Kafka, inside the same transaction as the events
  it produced. OAR reads from the matching engine publisher, not from Kafka, and its position is
  kept there (section 2). Moving it into Kafka would make the design work differently on Kafka and
  on Pulsar, which the interface in section 3 exists to prevent.
- **They would make every consumer depend on a setting.** Only a consumer reading with
  `isolation.level=read_committed` is protected. A consumer left at the default sees every event,
  including those of transactions that were abandoned. Consumers must discard duplicates anyway
  (R-0050), so transactions would add a second rule for them to get right, not replace the first.
- **They would lengthen the time to confirmation.** Each transaction ends with a commit, an extra
  exchange with the brokers, before its events are visible to those consumers. The time from
  publish to confirmation is what OAR's backlog depends on (section 13).

So the design accepts duplicates and makes them cheap to discard (section 11), on Kafka and Pulsar
alike, as [oar_bus_deduplication.md](oar_bus_deduplication.md) concludes.

---

## 9. Keeping events in order

Both Pulsar and Kafka keep events in order only within one partition. OAR publishes to a topic with
**one partition**: a non-partitioned topic in Pulsar, a topic created with one partition in Kafka.

**Why one partition, stated plainly.** Consumers need the events of the whole venue in one order, not
only the events of each instrument in order, and only one partition gives them that. Several
partitions keyed by instrument would keep each instrument's events in order, but the order between
instruments would be lost, and consumers could not rebuild it, because the sequence numbers on the
topic have gaps. The rest of this section explains each part of that.

Consumers of order events need three kinds of order:

- within an order, because a cancellation must follow the order it cancels;
- within an instrument, because anyone rebuilding the order book needs the order in which orders
  arrived at each price;
- across the whole venue, for anyone reconstructing exactly what happened, for example across one
  member's activity in several instruments.

One partition provides all three. Several partitions would provide only what the key preserves.
Consumers cannot reliably merge several partitions back into one order using the sequence number,
because the sequence numbers on the topic have gaps: the venue numbers other records, such as
execution reports, from the same sequence. A consumer therefore cannot tell whether it is waiting
for a missing event or whether that number was never going to appear on the topic.

The cost of one partition is that a single consuming application cannot spread its reading of the
topic across several processes. The capacity of one partition is commonly quoted in tens of
megabytes a second, which at a few hundred bytes an event is far above the venue's order rate. This
has not been measured here.

**If one partition is ever not enough.** Should the venue's event rate ever outgrow one partition,
which is not expected (see above), the choice would be between two losses, and it would be made
deliberately, not by adding partitions to the existing topic:

- **Several partitions, keyed by instrument.** Each instrument's events stay in order; the order
  across the venue is lost, and consumers that need it can no longer have it.
- **Several topics**, for example one for each group of instruments, each with one partition. The
  same loss, made visible in the topic layout.

The key is already the instrument, so that the first option keeps order within each instrument if it
is ever taken. But the number of partitions of a topic is fixed when the topic is created, and the
deploy step refuses a topic whose number of partitions differs (section 10.5): adding partitions to
a live topic moves keys between partitions, and order is lost at the moment of the change.

A Pulsar consumer that needs the events in order must use an Exclusive or Failover subscription.
A Shared subscription does not keep order.

---

## 10. Configuration

### 10.1 The principles

**Two different things are configured, at different times.** OAR's own configuration says how OAR
reaches the external messaging system and identifies itself, and OAR reads it when it starts. The
setup of the external messaging system says how each topic is kept: for how long, in how many
copies, and under which schema policies. The deploy step applies and checks that setup before OAR
starts. OAR never creates or changes a topic. A producer allowed to create topics creates one with
default settings whenever a name is mistyped.

**Settings that decide correctness are not configuration.** A setting that, if wrong, would lose
events, duplicate them unnecessarily or put them out of order is fixed in the code, with a comment
saying why. A value that must always be the same is not a choice, and offering it as one only
creates a way to get it wrong.

**Only what really differs between deployments is configured,** under names in this project's
vocabulary rather than the client library's property names. Each value is typed. Time limits are
duration strings such as `"30s"`, as elsewhere in the venue's configuration.

**There is no way to pass client library properties through.** librdkafka alone has well over a
hundred properties. A section that forwards any key straight to the library lets the configuration
override the settings that decide correctness, and nobody can tell from the file which keys matter.
If a deployment needs a different value for some tuning setting, and a measurement shows it, that
setting is added as a named setting.

**Everything is checked when the program starts.** An unknown key, a missing key, a malformed
address or a file that does not exist is reported with the file, the key and what was wrong, and
the program refuses to start. Nothing is left to be discovered at the first publish, where it would
appear as a client library error minutes later. Section 10.7 describes how unknown keys are found.

**Secrets are never in the file.** Passwords and tokens come from an environment variable, or from
a file whose name the configuration gives. Certificates and keys are given as file paths. This is
how the venue already handles its database password and the FIX gateways' certificates.

**A replaced certificate, key or password takes effect when the program restarts.** The client
libraries read them when the connection is made, so replacing one means restarting OAR. That is a
routine event: R-0052 requires that it does not stop trading, provided the restart fits within the
backlog bound (section 13). With two instances, the follower is restarted first, then the leader;
the leader's restart moves the lead to the follower.

**Each environment's values live in one place:** that environment's file under `environments/`,
through the `${...}` substitution every other component's configuration already uses.

**The program logs the configuration it is actually using** when it starts, with secrets replaced
by asterisks. The question "what is it actually connected to" is then answered by the log.

### 10.2 A program's own configuration

A program that uses the external messaging library has an `[external_messaging]` section. It says
which system to use, which declared topics the program produces to and consumes from, and how to
reach and authenticate to the system. Only the section for the chosen system needs to be present.

The topics are TOML arrays of declared topic names. There are two lists, because one program may
both produce and consume:

- `producer_topics`: the topics the program writes to;
- `consumer_topics`: the topics the program reads from.

Neither list contains full topic names. Every declared topic's full name is built from one naming
setting for the environment: a prefix for Kafka, or a tenant and namespace for Pulsar. Adding a
topic therefore means one declaration in the DSL and a release, and no configuration file in any
environment changes.

OAR's configuration, which only produces:

```toml
[external_messaging]
# Which implementation to use: "pulsar", "kafka" or "test". It must be one compiled into this
# binary; the program refuses to start and says which ones are available if it is not.
system = "${oar_external_messaging_system}"

# The identity every event carries, and that consumers use with the sequence number to recognise
# a duplicate. It must be the same on the primary and the secondary instance, so it comes from
# the environment file and never from the host name or the instance's role.
producer_identity = "${oar_producer_identity}"

producer_topics = ["order_events"]

# How long a publish may wait for the external messaging system to confirm it before it is treated
# as failed, and OAR resumes from the last confirmed sequence number. It must be comfortably
# shorter than the backlog bound after which trading halts (R-0053).
confirmation_timeout = "30s"

# How long OAR waits after a failed publish before it publishes again from the last confirmed
# sequence number (section 13.5). A tuning setting; it must be much shorter than the backlog bound, so
# that OAR has many attempts before trading halts.
retry_delay = "500ms"

# Back-pressure (section 13.4). The number of events OAR has handed to the client library and not yet
# had confirmed. Above the high watermark OAR stops reading from the matching engine publisher; below
# the low watermark it reads again. The high watermark must be below the client library's own limit
# on unconfirmed events, so that the library never has to refuse one. The values shown are
# placeholders; the real ones are chosen by measurement with the other tuning values (section 14,
# question 3).
unconfirmed_high_watermark = 50000
unconfirmed_low_watermark = 25000

[external_messaging.pulsar]
# One or more brokers, as a Pulsar service address.
service_url = "${oar_pulsar_service_url}"
# Every declared topic is created as persistent://<tenant>/<namespace>/<declared name>.
tenant = "${external_messaging_pulsar_tenant}"
namespace = "${external_messaging_pulsar_namespace}"
trusted_certificates_file = "${oar_pulsar_trusted_certificates_file}"
# "token" (a JSON Web Token held in a file) or "tls_certificate".
authentication = "token"
token_file = "${oar_pulsar_token_file}"

[external_messaging.kafka]
bootstrap_servers = ${oar_kafka_bootstrap_servers}
# Every declared topic is named <prefix><declared name>, for example "prod.venue.order_events".
topic_prefix = "${external_messaging_kafka_topic_prefix}"
trusted_certificates_file = "${oar_kafka_trusted_certificates_file}"
# "scram_sha_512" or "tls_certificate".
authentication = "scram_sha_512"
username = "${oar_kafka_username}"
password_environment_variable = "PUBSUB_OAR_KAFKA_PASSWORD"

[external_messaging.kafka.registry]
url = "${oar_kafka_registry_url}"
trusted_certificates_file = "${oar_kafka_registry_trusted_certificates_file}"
username = "${oar_kafka_registry_username}"
password_environment_variable = "PUBSUB_OAR_KAFKA_REGISTRY_PASSWORD"
# How long the startup check against the registry waits before logging a warning and carrying on.
startup_check_timeout = "5s"

# Written by the deploy step, never by hand: one entry for each message type the program writes.
[[external_messaging.kafka.schemas]]
message = "OrderEvent"
version = 3
registry_id = 17
```

Kerberos is not offered. Pulsar is authenticated with a token or a TLS client certificate; Kafka
with SCRAM, which the venue already uses for its own logins, or a TLS client certificate.

### 10.3 The settings a consumer adds

A program that consumes adds three settings, stated once for the program, not once for each topic:

```toml
[external_messaging]
consumer_topics = ["order_events"]
# What Kafka calls a consumer group and Pulsar calls a subscription. It must stay the same across
# restarts, because it is how the external messaging system remembers how far the program has read.
consumer_name = "oar_output_checker"
# Where to start reading when the external messaging system holds no position for this consumer:
# "oldest" (the oldest retained event) or "newest".
start_when_no_saved_position = "oldest"
```

The topic that holds events the consumer could not process (section 11) is named from the topic and
the consumer name, for example `order_events.oar_output_checker.dead_letter`, and needs no setting.
The Pulsar subscription type is not configurable: it is always Exclusive or Failover, because a
Shared subscription does not keep order.

### 10.4 What the program checks against the declared topics

When the program starts, it checks both topic lists against the declared topics and against its own
code, and refuses to start if they disagree:

- every listed topic must be declared;
- a producer topic's message type must be one the program is built to produce, and a consumer
  topic's message type must be one it can decode;
- every topic the program's code produces to must be listed.

The last check matters for OAR. If `order_events` were left out of OAR's `producer_topics`, OAR
would publish no order events at all, which is the loss R-0049 forbids. For OAR the list can never
be shorter than what the code does; its value is in the permissions it lets the deploy step grant
(section 10.5) and in what it tells a reader.

### 10.5 The setup of the external messaging system

The setup is divided between the deploy step and whoever runs the external messaging system.

**What the deploy step does.** For every declared topic, the deploy step creates the topic if it
is missing, and refuses to continue if an existing topic differs from what the design requires. It
never changes a live topic silently. Changing the number of partitions of an existing topic, for
example, would break ordering. The deploy step fixes these, and none of them is configurable:

- one partition, or a non-partitioned topic in Pulsar;
- the schema policies of section 8;
- the released schema versions, registered in order.

**Permissions.** From each program's `producer_topics` and `consumer_topics`, the deploy step grants
that program's identity permission to write exactly its producer topics and read exactly its
consumer topics, and nothing else. A producer is also granted permission to read its own producer
topics, because OAR finds where to resume by reading the last event it stored (section 12.1).

**What whoever runs the external messaging system decides.** How the system stores events is
decided and configured by whoever runs it, not by this project, because it depends on things
outside the venue: the machines the system runs on, and obligations such as how long records must
be kept for regulatory reasons. That covers two things:

- **Retention:** how long events are kept. In the development environment, where the project runs
  its own broker, retention is seven days.
- **Copies on separate machines.** Kafka and Pulsar each run on several machines, and store each
  event on more than one of them, so that an event is not lost when one machine fails. Two numbers
  control this: how many machines store each event, and how many of them must have stored it before
  the producer is told the event is stored. In Kafka these are the topic's replication factor and
  its `min.insync.replicas`; in Pulsar, the namespace's ensemble size, write quorum and
  acknowledgement quorum.

When the deploy step creates a topic, it creates it with the storage settings the system's
administrator has configured as defaults: in Kafka, the brokers' default replication factor; in
Pulsar, the namespace's policies.

**What the deploy step checks, without changing anything.** Two of the systems' defaults can lose
events that OAR has been told are stored, which is the loss R-0049 forbids. The deploy step reads
each topic's settings, and refuses to continue if either applies, saying what its administrator
must change:

- **Only one machine must store an event before it is confirmed.** Kafka's `min.insync.replicas`
  defaults to 1. With it, OAR can be told an event is stored while one machine holds it, and if that
  machine then fails, the event is gone after OAR has moved past it. The same applies to a Pulsar
  acknowledgement quorum of 1. A single-machine development environment is the exception, and says
  so in its environment file.
- **A Pulsar topic with no retention policy at all.** By default Pulsar deletes an event once every
  subscription has acknowledged it, and keeps nothing for a topic with no subscriptions. Events OAR
  publishes before any consumer subscribes would be discarded.

Every error the deploy step reports names the topic concerned.

### 10.6 How librdkafka's properties are set

The Kafka implementation is the only place where librdkafka's property names appear. It sets every
property from one of four sources, and the tables below say which. The defaults shown are those of
librdkafka's current `CONFIGURATION.md`; they change between versions, so they are checked again
against the version the build pins.

- **Fixed:** set in the code, with a comment giving the reason.
- **Named:** taken from a named setting in section 10.2 or 10.3.
- **Derived:** worked out from other settings.
- **Tuning:** set in the code at a chosen value, and made a named setting only if a measurement
  shows that some deployment needs a different value.

Connection and security:

| Property | librdkafka default | Source | Value |
|----------|--------------------|--------|-------|
| `bootstrap.servers` | empty | Named | `bootstrap_servers` |
| `client.id` | `rdkafka` | Derived | The producer identity and the instance, so the broker's logs show which process it is |
| `security.protocol` | `plaintext` | Derived | From `authentication` and the certificates; never unencrypted |
| `sasl.mechanisms` | `GSSAPI` (Kerberos) | Named | `authentication` |
| `sasl.username`, `sasl.password` | none | Named | `username`, and the password from `password_environment_variable` |
| `ssl.ca.location` | platform dependent | Named | `trusted_certificates_file` |
| `ssl.certificate.location`, `ssl.key.location` | none | Named | The client certificate and key, when certificate authentication is chosen |
| `ssl.endpoint.identification.algorithm` | `https` | Fixed | `https`: the broker's certificate must match its host name |
| `enable.ssl.certificate.verification` | true | Fixed | true |

Delivery, which decides correctness:

| Property | librdkafka default | Source | Value |
|----------|--------------------|--------|-------|
| `acks` | -1 (every in-sync copy) | Fixed | -1, set explicitly rather than relying on the default |
| `enable.idempotence` | false | Fixed | true; without it a retried send can land after later sends |
| `max.in.flight.requests.per.connection` | 1000000 | Fixed | 5, the most idempotence allows while keeping order |
| `message.send.max.retries` | 2147483647 | Fixed | Left effectively unlimited; the time limit below ends retrying |
| `enable.gapless.guarantee` | false | Fixed | true. When a failed batch could leave a gap, the producer stops with a fatal error instead of carrying on, which is the rule of section 2. librdkafka marks it experimental, so it is tested before being relied on. |
| `message.timeout.ms` | 300000 | Derived | From `confirmation_timeout` |
| `partitioner` | `consistent_random` | Fixed | Set so that the key always decides the partition, although with one partition it makes no difference |

Tuning:

| Property | librdkafka default | Source | Value |
|----------|--------------------|--------|-------|
| `linger.ms` | 5 | Tuning | How long librdkafka waits to fill a batch. It adds directly to the time before a confirmation, so it is chosen deliberately. |
| `queue.buffering.max.messages` | 100000 | Tuning | How many unconfirmed events librdkafka holds. OAR's `unconfirmed_high_watermark` is set below it, so OAR stops reading before the queue fills (section 13.4); if it fills all the same, a publish is refused and handled as section 13.5 describes. |
| `queue.buffering.max.kbytes` | 1048576 | Tuning | The same limit, by size |
| `batch.num.messages` | 10000 | Tuning | The default until a measurement says otherwise |
| `compression.codec` | none | Tuning | A deliberate choice, because every consumer must support whatever is chosen |
| `request.timeout.ms` | 30000 | Tuning | The default |
| `statistics.interval.ms` | 0 (off) | Tuning | Off unless the statistics are put to use |
| `debug` | empty | Named | A diagnostic setting. It switches on librdkafka's internal tracing when a problem is being investigated and affects nothing else. The output goes to the program's own log. |

A consumer adds:

| Property | librdkafka default | Source | Value |
|----------|--------------------|--------|-------|
| `group.id` | empty | Named | `consumer_name` |
| `auto.offset.reset` | `largest` (the newest) | Named | `start_when_no_saved_position` |
| `enable.auto.commit` | true | Fixed | false. With it true, librdkafka records a position in the background whether or not the event has been processed, which loses events the same way section 2 prevents on the producing side. |

### 10.7 How the configuration is read

Reading reuses what exists. `pubsub_itc_fw::TomlConfiguration` already reads strings, booleans,
integers, floating-point numbers, durations and arrays of tables. The framework already has loaders
for single sections that several components share, such as `MetricsConfigurationLoader`, and they
all read from the one `TomlConfiguration` object the component's own loader created. The external
messaging configuration follows the same pattern: an `ExternalMessagingConfiguration` structure and
an `ExternalMessagingConfigurationLoader`, in the external messaging library, so that every program
that produces or consumes gets the same code and the same checks.

`TomlConfiguration` needs one addition for this: reading an array of strings. It reads an array of
numbers into a `std::vector<double>`, but has nothing for strings, which `bootstrap_servers`,
`producer_topics` and `consumer_topics` need.

Refusing unknown keys, and reporting every problem at once, apply to every configuration file the
venue reads, not only to this one. They are described in
[configuration_files.md](../framework/configuration_files.md). The external messaging configuration
relies on both. Its loader declares the section for the system that was not chosen as present on
purpose, for example "external_messaging.pulsar: system is kafka".

`deploy.py` already covers the neighbouring case: it refuses to write a configuration that still
contains a `${...}` placeholder with no value.

### 10.8 The first implementation, and the sizes of the deployments

**Kafka is implemented first.** Its storage is simpler to reason about than Pulsar's, which adds a
separate storage layer (Apache BookKeeper) with its own rules. The interface stays independent of
the system, so the Pulsar implementation can follow without changing OAR.

The considerations that were weighed:

- On the target platform, RHEL8 with gcc 8.5, neither client is available at a usable version from
  Red Hat's repositories or EPEL, so both are built from release tarballs. librdkafka is the easier
  build: everything it depends on is packaged for RHEL8. RHEL8's own packaged librdkafka is version
  0.11.4, which is too old to use. The Pulsar C++ client also needs protobuf 3.20 or later, where
  RHEL8 packages 3.5.
- librdkafka 2.15.1 builds from its release tarball with gcc 8.5 in the Rocky 8 container, with no
  network access, using `scripts/build_librdkafka.sh`. It needs only zlib and OpenSSL 1.1 from the
  system. It is built with TLS and SCRAM, which librdkafka implements itself, and without Cyrus SASL,
  so Kerberos is not part of it.
- librdkafka does not run cleanly under ThreadSanitizer, so the Kafka implementation is not built in
  the ThreadSanitizer configuration. Section 3.3 explains how the hand-over of confirmations to OAR's
  thread is still checked there.
- A pull request to librdkafka cannot be approved until the contributor has signed Confluent's
  contributor licence agreement, whose text is not published where it can be read beforehand. The
  Pulsar C++ client is an Apache Software Foundation project, where an ordinary pull request needs
  no signature.
- Pulsar's registry is part of the broker. Kafka needs a separate registry process.

**The sizes of the deployments:**

| Environment | Kafka machines | What it survives |
|-------------|----------------|------------------|
| Development | 1 | Nothing. It exists to develop and test against, and its environment file says that it is a single machine, which is the one case where the deploy step accepts a single copy of each event as confirmed (section 10.5). |
| Production | 3, for now | The death of any one machine, without losing a confirmed event and without stopping publishing. |

On three machines, each machine runs both a Kafka broker, which stores events, and a KRaft
controller, which is one member of the group that agrees the cluster's metadata using the Raft
consensus algorithm. Three controllers keep a majority when one dies. With a replication factor of 3
and `min.insync.replicas` of 2, which are the administrator's settings (section 10.5), every event
is stored on all three machines, and confirmed once two have it. When one machine dies, the other
two still form a majority of controllers, and still satisfy `min.insync.replicas`, so publishing
continues. When two die, publishing stops rather than confirm an event that only one machine holds.

Whether three machines is the right minimum for production is still to be settled (section 14, question 4).

---

## 11. What consumers are expected to do

Consumers are not part of this project, but the design makes promises to them and asks certain
things of them.

- **Recognise duplicates** by the pair of producer identity and sequence number (R-0050). Every
  restart of OAR, and every change of OAR's leader, publishes some events again.

  **A consumer needs to remember only one number for each producer identity:** the highest sequence
  number it has processed. Any event whose sequence number is at or below it is a repeat, and is
  discarded. This holds because a topic has one partition (section 9), OAR publishes in the order
  it reads, and when it publishes again it starts from a position no later than its last confirmed
  event (R-0051), so every repeat comes after the event it repeats. It also relies on OAR publishing
  at most one event on a topic for each record it reads, which is true of every event kind in section
  4.4: each comes from one execution report. The sequence numbers on a topic have gaps (section 9),
  so a consumer must not expect each event's number to be one more than the last.
- **Decode with the writer's schema**, found from the event itself as described in section 8, and
  resolve it against the schema the consumer was built with. A consumer that decodes every event
  with its own built-in schema fails when the schema changes.
- **Never stop at an event that cannot be processed.** A consumer that retries such an event forever
  stops reading the partition behind it. The usual answer is to copy the event, unchanged, to a
  separate topic kept for events that could not be processed, wait until that copy is confirmed,
  and only then move past the event. Only permanent failures, such as an event that cannot be
  decoded, belong there. A temporary failure, such as a database that is briefly unavailable, is
  retried instead, or one outage fills that topic with good events. A consumer that sets an order's
  event aside must also set aside the later events for that order, or stop.

This project includes a test consumer that checks OAR's output and does each of these things. It is
part of the first implementation, not a later addition: it is how OAR's promises in this section are
checked.

---

## 12. Only the leader publishes {#oar_external_stream_which_publishes}

OAR runs as two instances, a primary and a secondary, like the venue's other components. **Only the
leader publishes.** Which instance leads is decided the way every other pair in the venue decides
it: by a majority of three voters granting leases, as
[majority_leases.md](../availability/majority_leases.md) describes. The voters are OAR's two
instances and the arbiter pool, and OAR uses the same `fix_common::PairLeaseAgent` as the other
components. So which instance publishes needs no mechanism of its own, and no configuration.

The rest of this design depends on there being one publisher at a time: the events on a topic
appear in the order OAR read them, and a consumer recognises a repeat with the one number described
in section 11. Leases give exactly that, because two instances never lead at once. A Pulsar broker
adds a second, independent check: it refuses a second producer with the same producer name while
the first is connected.

**When the leader stops leading**, because its lease ran out or its process died, it stops handing
events to the client library, discards what it has handed over but not had confirmed, and closes
its producer. Events already on their way to the external messaging system may still arrive after
that. They are duplicates, and consumers discard them.

**When an instance becomes leader**, it opens its producer and publishes from its resume position,
found as section 12.1 describes. With Pulsar there is one more thing to design (section 14, question
5): **waiting for the producer name.** The new leader's producer has the same name as the old one's,
and the broker refuses it while it still believes the old producer is connected, as
[oar_bus_deduplication.md](oar_bus_deduplication.md) records. The new leader keeps trying. How long
the broker takes to release the name counts towards the backlog bound in section 13, and must be
tested before it is relied on.

A configured switch, saying in each instance's configuration whether it may publish, is not needed
to decide which instance publishes. It could still be useful to keep an instance from ever leading,
for example while it is being repaired, but it would apply to leading as a whole, like the venue's
other components, not to publishing alone.

### 12.1 Where OAR resumes, whenever it starts

When OAR subscribes to the matching engine publisher, it names the sequence number to start from.
The publisher does not remember it for OAR. So OAR must find its position every time it starts
publishing: after a crash, after a supervised restart, when it becomes leader, and after a failed
publish (section 13.5). It finds it the same way every time.

**The external messaging system is the record of what was stored, so OAR asks it.** OAR resumes at
the record after the highest sequence number the external messaging system holds from OAR's producer
identity:

- **With Kafka,** a topic has one partition (section 9) and OAR is its only producer (section 7), so
  the last event on the topic is the last event OAR stored. OAR reads that one event when it starts,
  and takes the sequence number from its headers (section 8.2). This is why OAR is granted
  permission to read its own topic (section 10.5).
- **With Pulsar,** the broker reports the last sequence number stored under the producer name when
  OAR creates its producer.

This is exact: it includes events whose confirmation OAR never received before it stopped. Both of
OAR's instances find the same answer, so a restart and a change of leader are handled alike, and
nothing about the position is kept on OAR's machine. It needs the external messaging system to be
reachable, which costs nothing, because OAR cannot publish without it anyway; until it can reach it,
OAR waits.

**When the external messaging system holds no event from OAR**, on the very first start or if every
event has passed its retention period, OAR starts from the oldest record the matching engine
publisher still holds. That is always safe. R-0051 forbids resuming later than the last confirmed
event, never earlier, and the publisher never discards a record OAR has not acknowledged (section
13.4). Starting earlier only publishes events again, and consumers discard the repeats.

**One number is OAR's whole position**, although OAR reads two topics, the execution reports and
the technical events (section 4.4). Both are numbered from the sequencer's single sequence: a
technical event is sequenced by the sequencer like every other record
([trading_phases.md](../venue/trading_phases.md)). So a single sequence number says how far OAR has
got in both.

**This relies on the external messaging system storing OAR's events with no gaps.** Reading the last
stored event tells OAR where to resume only if everything before it was stored too. If events 101 to
105 were sent, 103 failed and 104 and 105 were stored, OAR would read 105, resume at 106, and 103
would be lost. With Kafka, `enable.gapless.guarantee` (section 10.6) makes the producer stop with a
fatal error in exactly that case, rather than carry on. Three checks must pass before this design is
relied on, and they are listed with open question 5 (section 14):

- that Kafka, with `enable.gapless.guarantee`, never stores a later event after an earlier one failed;
- that reading the last event of a one-partition Kafka topic works as described, with librdkafka;
- that a Pulsar broker reports the last sequence number for a producer name when its own removal of
  duplicates is switched off, which this design does not depend on
  ([oar_bus_deduplication.md](oar_bus_deduplication.md)), and that a Pulsar producer gives the same
  guarantee against gaps.

---

## 13. When publishing stops {#oar_external_stream_when_publishing_stops}

OAR's availability decides whether the venue trades. Two requirements say how:

- **A restart of OAR does not stop trading (R-0052).** The venue goes on accepting orders while
  OAR restarts and catches up.
- **Where external publishing cannot resume, trading is halted (R-0053).** When no instance of OAR
  is publishing and the backlog of records not yet published passes a stated bound, the venue
  halts trading and says that it has.

### 13.1 The backlog bound

**The bound is measured, not chosen.** It must be longer than every interruption the venue is meant
to ride through, or it contradicts R-0052. So it is set above the longest of these, with a margin:

- a supervised restart of the leader;
- the catch-up after that restart, publishing the records that built up while it was down, at the
  venue's highest order rate;
- the other instance becoming leader (section 12), including, with Pulsar, the time the broker takes to
  release the producer name.

Each is measured on the production hardware, and measured again whenever OAR, the client library or
the external messaging system changes, as the restart period of the venue's other components is
(R-0081). The bound can be stated as a number of records or as the age of the oldest record not yet
published; which is to be decided with the measurements. The confirmation timeout in section 10.2
must be well inside the bound, so that a publish that is never confirmed is retried before the
bound is reached.

**What it means for the size of the external messaging system.** With three Kafka machines,
publishing continues when one dies and stops when two die (section 10.8). If the two are not back
before the bound passes, trading halts. That is the consequence open decision 4 must weigh.

### 13.2 Which component halts trading

The matching engine publisher is the component that can measure both parts of R-0053's condition:
whether OAR is connected to it, and how far behind OAR's acknowledgements are. So it decides when
the bound has passed. The halt itself is a venue-wide trading halt, which the venue cannot yet
declare ([BUG-0065](../bug_list.md#bug_0065)). R-0053 therefore cannot be met until that is built.

The decision is made inside the venue from its own measurements. It must not depend on the metrics
described below, because collecting metrics is optional.

### 13.3 What operators can see

OAR and the matching engine publisher report, as metrics:

- the backlog: how many records OAR has not had confirmed, and the age of the oldest of them,
  alongside the bound;
- how long each publish takes to be confirmed, as a histogram;
- how many publishes have failed or timed out.

They also log, at the time it happens:

- each confirmation failure or timeout, with the reason the client library gave;
- which instance leads, and so publishes, and each change of leader;
- the result of the startup checks against the registry (section 8).


### 13.4 Back-pressure: when OAR stops reading

OAR counts the events it has handed to the client library and not yet had confirmed. When the count
rises above `unconfirmed_high_watermark` (section 10.2), OAR stops reading from the matching engine
publisher; when it falls below `unconfirmed_low_watermark`, OAR reads again. The high watermark is
below the client library's own limit on unconfirmed events, so the library does not have to refuse
an event because it is full.

Stopping reading is the whole of the mechanism. OAR sends nothing to the publisher and does not
unsubscribe. The venue's pub/sub already handles a subscriber that stops reading
([pubsub.md](pubsub.md)): the subscriber's socket fills, a send returns `EAGAIN`, and the publisher
stops sending to that subscriber until the socket can be written again. Nothing builds up inside OAR
while it is not reading; the records wait in the publisher's log.

**OAR's acknowledged position always holds back the publisher's log.** The pub/sub design limits
how far behind a subscriber may fall: past a retention window, the publisher discards old records
anyway, disconnects the subscriber, and on reconnection tells it where its stream now begins. For
other subscribers that gap is explicit and recoverable. For OAR it would be a permanent loss of
events, which R-0049 forbids, and a long outage of the external messaging system, with OAR not
reading, is exactly what would cause it. So the retention window does not apply to OAR: the
publisher keeps every record OAR has not acknowledged, however long that is. How far behind OAR can
fall is limited instead by the backlog bound, after which trading halts (section 13.1).

### 13.5 When a publish fails

**A failure reported after the event was handed over.** The client library retries by itself: during
a short outage, librdkafka keeps the event and keeps sending it, for up to `message.timeout.ms`, which
is derived from `confirmation_timeout` (section 10.6). An outage shorter than that is absorbed
there, and OAR sees only confirmations arriving late. When the time runs out, the library reports
the event as failed, and section 2's rule applies. OAR forgets every event it has handed over and not
had confirmed, waits `retry_delay`, subscribes to the publisher again at the record after the last
confirmed one (section 12.1), and publishes those records again.

What OAR forgets is only its own copy of each event. The records they came from are still in the
publisher's log, because OAR acknowledges a record only once its event is confirmed (section 2) and
the publisher keeps every record OAR has not acknowledged (section 13.4). **The publisher's log is
OAR's retry queue.** Unlike a queue in OAR's memory, it survives a crash of OAR, a restart and a change
of leader, and reading it again keeps the events in order. Some of the events reported as failed may
in fact have been stored; publishing them again makes duplicates, which consumers discard.

**A failure of the call that hands an event over.** Here the event has been read from the publisher
and the call to hand it to librdkafka fails at once. Receiving a record does not use it up; only
OAR's acknowledgement does. So the record is still both in OAR's hand and in the publisher's log, and
OAR never lets go of an event until it is confirmed: it either holds it and tries again, or drops its
own copy and reads it again from the publisher. librdkafka's produce call fails for these reasons:

| Failure | What it means | What OAR does |
|---------|---------------|---------------|
| `QUEUE_FULL` (`ENOBUFS`) | librdkafka already holds its limit of unconfirmed events | Keeps the event in hand, stops reading, lets the helper thread collect confirmations, and hands the same event over again. Only this one event ever waits, because OAR reads nothing more until it is handed over. With the high watermark below the limit (section 13.4) this should not happen. |
| `MSG_SIZE_TOO_LARGE` (`EMSGSIZE`) | The event is larger than the configured maximum | Stops publishing, keeping the event, and reports an error naming it. Trying again cannot help, and skipping it is forbidden (R-0049), so unless someone intervenes the backlog passes the bound and trading halts (section 13.1). This is prevented before OAR goes into service: the largest possible event is limited by the schema's fields, and is checked against the size limit. |
| `UNKNOWN_TOPIC` (`ENOENT`), `UNKNOWN_PARTITION` (`ESRCH`) | The cluster says the topic or partition does not exist | Stops publishing, keeping the event, reports an error, and tries again every `retry_delay`, in case the topic is restored. The deploy step creates topics and OAR checks them when it starts (section 10.4), so this means something outside OAR is wrong. |
| `FATAL` (`ECANCELED`) | The producer is permanently broken, for example because `enable.gapless.guarantee` detected a gap | Discards the broken producer and creates a new one, forgets every unconfirmed event, including the one in hand, and publishes again from the record after the last confirmed one (section 12.1). |
| `STATE` (`ENOEXEC`) | A Kafka transaction forbids producing | Cannot happen, because OAR uses no transactions (section 8.2). If it does, it is a defect, handled as `FATAL`. |

**Repeated failures need no special handling.** OAR keeps trying, `retry_delay` apart. If it cannot
succeed, the backlog passes the bound and trading halts (section 13.1); that is the escalation.

### 13.6 The end of the trading day

OAR is not stopped during a trading day. The venue's software starts afresh each day, and stops when
end-of-day processing is complete; maintenance and upgrades are done when the venue is not trading.
During the day, then, OAR stops only by accident, and sections 2 and 12.1 cover that.

**The end of day is not complete until OAR reports that every event of the day is confirmed.** OAR
learns that the day is ending from the `eod` technical event, which it follows (section 4.4). The
`eod` event is sequenced by the sequencer, so it has a definite place among the execution reports.
When every event up to it has been confirmed, OAR reports that it has finished the day. Only then is
end-of-day processing complete, and only then is OAR stopped. OAR may be the component that declares
the end of day finished; how its report combines with the other work of the `eod` phase, such as
receiving instrument prices for the next day, belongs to the venue's end-of-day design
([trading_phases.md](../venue/trading_phases.md)).

This relies on no order record ever following the `eod` event, which must be settled with the trading
phases: whether a member may cancel an order during `eod` is still open there.

**OAR waits as long as it takes, and never finishes quietly with events unconfirmed.** No trading is
waiting on it, so there is no time limit. If the external messaging system cannot confirm, OAR reports
the problem and keeps trying until the events are confirmed or someone intervenes. The waiting is part
of OAR's normal work, done before the process is told to stop, so the framework's limit on how long a
thread may take to stop does not come into it. OAR does not call `rd_kafka_flush()` to wait, because
librdkafka delivers the confirmations it collects to whichever thread calls it; OAR waits until its
count of unconfirmed events reaches zero, while the helper thread goes on collecting them (section
3.3).

If OAR is told to stop at any other time, it stops reading, closes its producer and finishes. Nothing
is lost: whatever was unconfirmed is published again when it next starts (section 12.1).

---

## 14. Open decisions {#oar_external_stream_open}

1. **Which registry to use with Kafka.** Apicurio Registry (Java) or Karapace (Python). Whether
   either installs from a release tarball without network access, and runs on RHEL8, has not been
   checked.

2. **How the member is identified.** Section 4.5 follows the execution report, which identifies the
   member through `account` and the parties group. This is to be confirmed against what consumers of
   such a stream actually need.

3. **The tuning values for the Kafka implementation** (section 10.6), chosen from measurement: how
   long librdkafka waits to fill a batch, how many unconfirmed events it may hold, and whether events
   are compressed. Before measuring, the decision must state what it is aiming for: the longest time
   from publish to confirmation that is acceptable, and the highest rate of events that must be
   sustained, with the venue's peak order rate as the starting point. It must also state how they are
   measured, on which machines and against which deployment of Kafka, so that a later measurement
   can be compared with it.

4. **The minimum size of a production deployment.** Three machines for now (section 10.8). Whether
   that is enough, how split-brain is prevented at each layer, and what Pulsar would need, given
   its separate storage and metadata layers, is still to be settled. It must be weighed against
   section 13: whatever stops the external messaging system confirming events for longer than the
   backlog bound halts trading.

5. **Before section 12.1 is relied on.** Three checks, by experiment: that Kafka with
   `enable.gapless.guarantee` never stores a later event after an earlier one failed; that reading the
   last event of a one-partition Kafka topic works as described; and that a Pulsar broker reports the
   last sequence number for a producer name with its removal of duplicates switched off, and stores a
   producer's events with no gaps. With Pulsar, also how a new leader waits for the broker to release
   the producer name (section 12), and how long that takes.