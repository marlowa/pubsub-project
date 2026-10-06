# Configuration files, and refusing unknown keys {#configuration_files}

**Status: the reading of configuration files described in section 1 exists. Everything from section
2 onwards is design, not yet implemented.**

This document applies to every TOML file the venue reads: the configuration of each C++ component,
and the files the Python scripts in `scripts/` read, such as the environment files under
`environments/`.

---

## 1. How a component reads its configuration

Each C++ component has a loader, for example `MatchingEnginePublisherConfigurationLoader`. The
loader creates one `pubsub_itc_fw::TomlConfiguration` object, loads the component's file into it,
and reads each key into a structure that the component then uses. Keys are named by their full
dotted path, such as `ha.peer_port`. An entry in an array of tables is named with an index, such as
`credential[0].comp_id`.

Sections that several components share are read by shared loaders, such as
`MetricsConfigurationLoader` and `LoggingConfigurationLoader`. The component's loader passes its
`TomlConfiguration` object to each of them, so every key in the file is read through that one
object.

A component that reads a second file uses a second object for it. The authentication service
is the one that does: its credentials file is loaded into a `TomlConfiguration` of its own.

All reading happens while the configuration is loaded. Each loader returns a filled structure, and
nothing reads the file afterwards.

---

## 2. The problem

Nothing notices a key that no loader reads. A key whose name is mistyped, for example
`heartbeat_timout_seconds`, is simply never looked up. If the loader treats the key as optional,
the component starts with the default value, and the value the operator wrote is silently ignored.
The same happens to a key that a loader used to read and no longer does, and to a key copied from
another component's file where it had a meaning.

The failure appears later, as behaviour nobody configured, and nothing points back to the file.

---

## 3. The design: record what is read, and report what is not

### 3.1 The mechanism

`TomlConfiguration` records the full name of every key that any loader reads. After all loading is
complete, it lists every key present in the file that is not in that record. If the list is not
empty, the component refuses to start, and the error names every such key with its file:

```
matching_engine_publisher_primary.toml: unknown keys: ha.heartbeat_timout_seconds, wal.segment_sise
```

"Reads" here means any lookup of a key that is present, whether the loader then uses the value or
not. A lookup of a key that is absent records nothing, because there is nothing in the file to
account for.

### 3.2 Why the record of reads, and not a list of known keys

The obvious alternative is for each loader to declare a list of the keys it knows, and to check the
file against that list. That writes every key's name twice: once in the list, and once where the
key is read. The two copies drift apart. The dangerous drift is a name that stays in the list after
the code stops reading the key: the file is then accepted, and the key silently ignored, which is
exactly the fault the check exists to prevent.

With a record of reads, the reads themselves define which keys are known. There is no second copy
to keep in step with the code.

### 3.3 When the check runs

The component's own loader runs the check once, at the very end, after every shared loader has run.
All the loaders share one `TomlConfiguration` object, so a key read by `MetricsConfigurationLoader`
counts as read, although the component's loader never touched it itself. A check run earlier would
report keys that a shared loader had not yet reached.

### 3.4 Keys that are present on purpose but not read

Some keys are legitimately present in a file but not read in a particular configuration:

- the high availability keys, when high availability is switched off;
- a section for an alternative that was not chosen, such as the Pulsar section of a program whose
  configuration selects Kafka;
- the rest of an optional section whose loader stops early because the section is switched off.

The loader must say so explicitly. It declares the section or key, and the reason, for example:

```
ha: ha_enabled is false
external_messaging.pulsar: system is kafka
```

Each such declaration is logged at Info when the component starts, so the log shows what was
ignored and why. Nothing is ignored without a stated reason. A declaration covers the named key, or
every key under the named section.

### 3.5 The one rule this depends on

All reading must happen while the configuration is loaded. A key read later, on some other code
path, would be reported as unknown although the component does use it. Section 1 describes this as
how the loaders already work, so the rule states current practice rather than imposing something
new. It is recorded here because the check makes it load-bearing.

---

## 4. Reporting every problem at once

The existing loaders stop at the first problem, by throwing `ConfigurationException`. An operator
with three mistakes in a file then fixes them one restart at a time.

The loaders will instead collect problems as they go: missing keys, values of the wrong type,
values out of range, files that do not exist. At the end, the unknown keys are added to the same
collection, and every problem is reported together before the component refuses to start. A problem
that makes further checking meaningless, such as a file that cannot be parsed at all, is still
reported alone.

---

## 5. The files the Python scripts read

The same principle applies to the TOML files read by the scripts in `scripts/`, such as
`deploy.py` and `devenv.py`. Python's `tomllib` returns an ordinary dictionary. The scripts will read
it through a small wrapper that records each key looked up and reports the keys never looked up, in
the same form as section 3.1, with the same explicit declarations for keys present on purpose.

The environment files under `environments/` are a special case, because most of their values are
not read by a script directly. They are substituted into the `${...}` placeholders of the
component templates. `deploy.py` already refuses to write a configuration that still contains a
placeholder with no value. The opposite direction is not checked: a value defined in an environment
file that no template uses. Such a value is most often a mistyped name, and the placeholder it was
meant for is then reported as undefined, so the mistake is already caught from that side. A value
that no template uses at all is reported as a warning, because it is a leftover that no longer
affects anything.

---

## 6. Testing the check

Each part is tested by making it fail on purpose:

- a file with a mistyped key must stop the component and name the key;
- a file that uses every key must pass;
- each declaration of a key present on purpose must be tested with its condition both true and
  false: with high availability off, the high availability keys are accepted; with it on, they must
  all be read, and a mistyped one must be reported;
- a file with several problems must report all of them together.

---

## 7. Introducing the check

Applied to every component, the check will very probably find keys in the existing templates that
no loader reads any more. So it is introduced in two steps:

1. The check is first run over every template, as `deploy.py` renders them for each environment,
   and reports what it finds without refusing anything. Each key it finds is either corrected,
   removed from the template, or given a declaration under section 3.4 with its reason.
2. Once every template passes, the check refuses to start any component whose file contains an
   unknown key.
