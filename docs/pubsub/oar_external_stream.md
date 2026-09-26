# The order activity recorder's external stream {#oar_external_stream}

**Status: design, not implemented.** Several decisions are still open. They are listed together in
[section 12](#oar_external_stream_open), and the sections that depend on them say so.

---

## 1. What this document covers

The order activity recorder (OAR) subscribes to the topics of the matching engine publisher and
publishes an event for each order to a system outside the venue. That system is called the
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
  is set up.

Related documents:

- The requirements OAR must meet are R-0049 to R-0053 in the functional specification
  (`docs/book`, the order activity recorder subsection of the high availability chapter).
- [oar_bus_deduplication.md](oar_bus_deduplication.md) records the evidence on whether the outside
  system can be relied on to discard duplicate messages. It concludes that it cannot, and this
  document follows that conclusion.
- Section 7 of [mep_oar.md](mep_oar.md) describes OAR's place alongside the matching engine
  publisher. Where the two documents differ on how OAR publishes externally, this document is the
  current design.

---

## 2. The rule every part of this design keeps

OAR saves its position, the sequence number of the last record it has finished with, **only after
the external messaging system has confirmed that it has stored the event, and never before.**

The reason is the difference between the two ways a crash can go wrong:

- If OAR crashes after publishing an event but before saving its position, it publishes that event
  again when it restarts. The event appears twice downstream. A consumer can recognise the second
  copy and discard it, because every event carries an identity (R-0050).
- If OAR saved its position first and then crashed before the event was stored, the event would
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
   The duplicates this causes are expected and are handled downstream.

---

## 3. The external messaging library

The interface and its implementations live in a library of their own, separate from the framework
library `pubsub_itc_fw`. Its working name is `external_messaging`. The
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
promoted secondary must present the same identity the primary did.

If a single record from the matching engine publisher ever produces events on more than one topic,
that record counts as confirmed only when all of its events are confirmed, and OAR's saved position
must not pass it before then.

Inside the Pulsar implementation there is one Pulsar producer for each topic, because a Pulsar
producer is bound to a single topic. A Kafka producer can send to any number of topics.

### 3.3 Confirmations and threads

Both the Pulsar and Kafka client libraries run threads of their own and report confirmations by
calling a function on one of those threads. OAR's state belongs to OAR's own thread and must not be
touched from any other thread.

So an implementation never passes a confirmation to OAR directly. It places the confirmation on
OAR's inter-thread queue, and OAR handles it on its own thread, like any other message. OAR sees
confirmations and failures as ordinary events on its own thread and never learns that another thread
was involved.

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

An `OrderEvent` has three groups of fields.

**Fields every event has.**

| Field | Contents |
|-------|----------|
| `seq_no` | The sequence number the venue assigned to the record this event came from. |
| `producer_id` | OAR's producer identity (section 3.2). |
| `event_kind` | Which kind of event this is. See below. |
| `symbol` | The instrument. |
| `cl_ord_id` | The member's identifier for the order or request (FIX `ClOrdID`). |
| `venue_time_ns` | When the venue recorded the record, in nanoseconds since the Unix epoch. |
| `published_time_ns` | When OAR published the event, in nanoseconds since the Unix epoch. |

**The kind of event.** `event_kind` is an Avro enum with the symbols `Unknown`, `Added`, `Amended`
and `Cancelled`, and a default symbol of `Unknown`. The default matters for schema evolution. When
a program built against an older version of the schema meets a symbol it does not know, the Avro
library gives it the default instead of failing. A kind of event added in a later version therefore
reaches older programs as `Unknown`, and they can skip it.

`Amended` is in the schema from its first version, although the venue cannot yet amend an order.
It costs nothing now, and it means amendment needs no change to the kinds of event when the venue
supports it.

**Fields that apply to some kinds of event only.** Each of these is declared as either null or a
value, with a default of null. For example:

| Field | Filled in for |
|-------|---------------|
| `orig_cl_ord_id` | `Amended` and `Cancelled`: the identifier of the order being changed (FIX `OrigClOrdID`). |
| `side` | All kinds. |
| `ord_type` | `Added` and `Amended`. |
| `order_qty` | All kinds. |
| `price` | `Added` and `Amended`, for orders that have a price. |
| `time_in_force` | `Added` and `Amended`. |
| `account` | Whenever the member supplied one. |
| `transact_time_ns` | Whenever the member supplied it: the member's own time for the order or request. |

The final list of fields depends on two open decisions: whether events record requests, outcomes
or both, and where the member's identity comes from (section 12, questions 4 and 5).

**Why one record with optional fields, and not a choice between several record types.** Avro also
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
text. Whether `OrderEvent` keeps them as strings or uses Avro's `decimal` type is open (section 12,
question 6).

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
open (section 12, question 3).

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

---

## 9. Keeping events in order

Both Pulsar and Kafka keep events in order only within one partition. OAR publishes to a topic with
**one partition**: a non-partitioned topic in Pulsar, a topic created with one partition in Kafka.

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

The key is still the instrument. Changing the number of partitions of an existing topic moves keys
between partitions, and order is lost at the moment of the change, so the number of partitions is
fixed when the topic is created. If a topic with several partitions is ever needed, keying by
instrument means order within each instrument is what it keeps.

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

The deploy step reads this from the environment file, applies it to every declared topic, and checks
it. It creates whatever is missing, and refuses to continue if an existing topic differs from what
is required. It never changes a live topic silently. Changing the number of partitions of an
existing topic, for example, would break ordering.

**Fixed by the deploy step, not configurable:** one partition, or a non-partitioned topic in
Pulsar; the schema policies of section 8; the released schema versions, registered in order.

**Permissions.** From each program's `producer_topics` and `consumer_topics`, the deploy step grants
that program's identity permission to write exactly its producer topics and read exactly its
consumer topics, and nothing else.

**Configured for each environment:**

```toml
[external_messaging_setup]
# How long events are kept. This is a policy decision about the record the venue keeps outside
# itself, not a technical one, and whoever owns that policy has to state it.
retention = "${external_messaging_retention}"

# How many copies of each event are kept, and how many must hold it before it counts as stored.
# A single-machine development environment can only have one; production needs three and two, so
# that one machine can fail without losing a confirmed event or stopping publishing.
copies_kept = 3
copies_required_for_confirmation = 2

# Only for topics whose retention differs from the default above. The key is the declared name.
[external_messaging_setup.retention_overrides]
order_events = "${external_messaging_retention_order_events}"
```

The deploy step translates the two settings about copies into each system's terms: for Kafka, the
topic's replication factor and its `min.insync.replicas`; for Pulsar, the namespace's ensemble
size, write quorum and acknowledgement quorum. Kafka's `min.insync.replicas` defaults to 1, and with
it an event can be confirmed while only one machine holds it.

Pulsar's retention needs particular care. By default Pulsar deletes an event once every subscription
has acknowledged it, and keeps nothing for a topic with no subscriptions. Without a retention policy,
events OAR publishes before any consumer subscribes are discarded. The deploy step therefore always
sets retention, and refuses an environment that does not state it.

An override that names a topic that is not declared is an error. Every error names the topic
concerned.

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
| `queue.buffering.max.messages` | 100000 | Tuning | How many unconfirmed events librdkafka holds. When it is full a publish is refused, and OAR stops reading from the matching engine publisher until confirmations catch up. |
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

### 10.7 How the configuration is read, and how unknown keys are found

Reading reuses what exists. `pubsub_itc_fw::TomlConfiguration` already reads strings, booleans,
integers, floating-point numbers, durations and arrays of tables. The framework already has loaders
for single sections that several components share, such as `MetricsConfigurationLoader`, and they
all read from the one `TomlConfiguration` object the component's own loader created. The external
messaging configuration follows the same pattern: an `ExternalMessagingConfiguration` structure and
an `ExternalMessagingConfigurationLoader`, in the external messaging library, so that every program
that produces or consumes gets the same code and the same checks.

Three things are missing from `TomlConfiguration`:

1. **Arrays of strings.** It reads an array of numbers into a `std::vector<double>`, but has nothing
   for strings, which `bootstrap_servers`, `producer_topics` and `consumer_topics` need.
2. **Finding unknown keys.** Nothing currently notices a key that no loader reads, so a mistyped key
   is silently ignored.
3. **Reporting every problem at once.** The existing loaders stop at the first problem.

**The proposed way of finding unknown keys** is for `TomlConfiguration` to record the full name of
every key that any loader reads, and to report every key in the file that nobody read.

- **The reads are the definition of what is known.** There is no separate list of known keys to
  keep in step with the code. A separate list repeats every key name, and a name that stays in the
  list after the code stops reading it would be accepted and ignored, which is the very fault being
  prevented.
- **The check runs once, at the end.** The component's own loader calls it after every section
  loader has run, because they all share the one `TomlConfiguration` object. All the unread keys
  are reported together, each with its full name, for example
  `external_messaging.kafka.sasl.mechanism`.
- **A key that is deliberately unused must be said to be.** Some keys are legitimately present but
  not read: the Pulsar section when the system is Kafka, or the high availability keys when high
  availability is switched off. The loader declares these explicitly, naming the section or key and
  the reason, for example "external_messaging.pulsar: system is kafka". Each such declaration is
  logged at Info when the program starts, so the log shows what was ignored and why.
- **All reading happens while loading.** A key read only later, on some other code path, would be
  reported as unknown. That is already how the loaders work: each one returns a filled structure,
  and nothing reads the file afterwards.

The check is tested by making it fail on purpose: a configuration with a mistyped key must stop the
program and name the key. A configuration using every key must pass. Each declared exception must
be tested with its condition both true and false.

Applied to the whole venue, the check will probably find keys in the existing templates that no
loader reads any more. So the check is first run over every template, as `deploy.py` renders them,
and anything it finds is corrected before refusal is switched on. Whether the check, and reporting
every problem at once, are adopted for the whole venue or only for the external messaging
configuration is open (section 12, question 7).

`deploy.py` already covers the neighbouring case: it refuses to write a configuration that still
contains a `${...}` placeholder with no value.

## 11. What consumers are expected to do

Consumers are not part of this project, but the design makes promises to them and asks certain
things of them.

- **Recognise duplicates** by the pair of producer identity and sequence number (R-0050). Every
  restart and every promotion of OAR publishes some events again.
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

This project includes a test consumer that checks OAR's output and does each of these things.

---

## 12. Open decisions {#oar_external_stream_open}

1. **Pulsar or Kafka for the first implementation.**
   - On the target platform, RHEL8 with gcc 8.5, neither client is available at a usable version
     from Red Hat's repositories or EPEL. Both must be built from release tarballs.
   - librdkafka is the easier build: everything it depends on is packaged for RHEL8. RHEL8's own
     packaged librdkafka is version 0.11.4, which is too old to use.
   - The Pulsar C++ client needs protobuf 3.20 or later, and RHEL8 packages 3.5, so protobuf is a
     second tarball build. Whether the current client builds with gcc 8.5 has not been checked.
   - librdkafka does not run cleanly under ThreadSanitizer, so the Kafka implementation is not built
     in the ThreadSanitizer configuration.
   - A pull request to librdkafka cannot be approved until the contributor has signed Confluent's
     contributor licence agreement, whose text is not published where it can be read beforehand.
     The Pulsar C++ client is an Apache Software Foundation project, where an ordinary pull request
     needs no signature.
   - Pulsar's registry is part of the broker. Kafka needs a separate registry process.

2. **The library's name.** The working name is `external_messaging`.

3. **Which registry, if Kafka is used.** Apicurio Registry (Java) or Karapace (Python). Whether
   either installs from a release tarball without network access, and runs on RHEL8, has not been
   checked.

4. **Whether events record requests, outcomes, or both.** The `orders` topic carries requests: a
   `NewOrderSingle` and an `OrderCancelRequest`. A cancellation request can be refused, for example
   when the order has already been filled. Some cancellations are made by the venue itself and
   appear only as execution reports, never as a request on the `orders` topic. R-0049 requires
   every record the venue produces to reach the external messaging system. Whether a `Cancelled` event means
   "a cancellation was requested" or "the order was cancelled" must be settled before the fields of
   `OrderEvent` are final, and it decides whether OAR publishes events derived from execution
   reports.

5. **Where the member's identity comes from.** An `OrderEvent` should say which member an order
   belongs to. The order messages on the `orders` topic do not carry it as a field of their own.
   Where OAR obtains it has not been established.

6. **How prices and quantities are represented.** As strings, which is how the venue's messages
   carry them, or as Avro's `decimal` type, which consumers can use as numbers without parsing but
   which fixes a precision and scale in the schema.

7. **Whether unknown keys are refused, and every problem reported at once, for the whole venue.**
   Both are needed for the external messaging configuration (section 10.7). Applying them to every
   component changes how all the existing loaders work, and will need the existing templates to be
   corrected first.

8. **The tuning values for the Kafka implementation** (section 10.6), chosen from measurement:
   how long librdkafka waits to fill a batch, how many unconfirmed events it may hold, and whether
   events are compressed.
