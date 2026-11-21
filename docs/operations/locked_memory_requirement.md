# The limit on locked memory {#locked_memory_requirement}

> **One rule.** The user the venue runs as must be allowed to lock at least as much memory as the matching engine or
> the sequencer locks, whichever is more. Each matching engine locks its order book's map, its open-order region and,
> with high availability on, the store of reports it keeps while following the WAL, about 65 GB in production. Each
> sequencer locks its record of command identifiers, 6 GiB in production. Each process has its own limit, so the
> figure is per process, not for a pair. Without it, the process refuses to start.

## Why the engine locks its memory

The matching engine keeps its open orders in the open-order region, a file mapped into its memory, and finds each one
through a map from the order's identity to its record, which is ordinary memory. Linux hands out addresses when memory
is asked for, and supplies real memory only when each page is first written, without checking that it will be there.
An engine that only reserved its map would find out that the machine was short of memory in the middle of the day,
when the kernel's out-of-memory killer ended it in the middle of an order.

An engine with high availability on also keeps, while it follows the WAL, its own reports of the commands it applies,
until it can compare them with the leading engine's. They are kept in a store allocated at start: 64 MiB of encoded
reports and a ring of 100,000 entries saying where each lies, 73 MB in all.

So before it reports itself ready, the engine locks all of these into memory (`mlock`). The kernel supplies every page at once
and keeps each one in memory from then on, never swapping it out or dropping it. If the memory is not there, or the
engine is not allowed to lock that much, locking fails with an error, and the engine refuses to start, at the start of
the day, before any trading. The design is in
[the_engine_holds_its_memory_from_the_start.md](../venue/the_engine_holds_its_memory_from_the_start.md).

## What the engine logs

At start, before it locks anything, at Info:

```
MatchingEngineThread: locking its memory -- 72 MB for the order book's map, 503 MB for its region and 73 MB for the reports kept while following, 649 MB in all, against a limit on locked memory of 3980 MB
```

and once it has locked them:

```
MatchingEngineThread: its memory is locked, 649 MB, in 41 ms
```

The figures above are from the development environment, which allows 2 million open orders and reserves the map for
262,144. In production, with both sized for 100 million, the map is 38.9 GB, the region 26.4 GB and the kept reports
73 MB, 65.4 GB in all. An engine with high availability off locks no store of kept reports, and logs 0 MB for it.
Locking took 39 to 43 milliseconds for 649 MB on the development machine; at the same rate production's 65 GB takes
about 4 seconds, which has not been measured on a production machine.

If the limit is lower than the figure, the engine logs at Critical and exits with status 1, as it does for its other
refusals to start, without spending any time locking:

```
MatchingEngineThread: refusing to start -- it needs 649 MB of locked memory, and this process's limit on locked memory (RLIMIT_MEMLOCK) is 1 MB. ...
```

If the limit is high enough but locking still fails, because the machine does not have the memory free, the engine
logs at Critical which part could not be locked and why, and exits with status 1 in the same way.

## Setting the limit

The limit is `RLIMIT_MEMLOCK`, shown in kilobytes by `ulimit -l`. On many Linux machines it is small by default; on the
development machine it is about 4 GB. For the user the venue runs as, set it in a file under `/etc/security/limits.d`,
for example, for a user named `pubsub`:

```
pubsub  soft  memlock  unlimited
pubsub  hard  memlock  unlimited
```

or to a number of kilobytes at least the figure the engine logs. The file is read at login, so a launcher started from
a session that began before the change keeps the old limit. Whatever starts the launcher in some other way must set
the limit itself, for example with `ulimit -l` in the script that starts it. A process running as root may lock memory
whatever its limit, and the engine does not compare the figure with the limit then.

A container has a limit of its own, which does not come from the host's files. Docker's default is 8 MiB, so a
container that runs the venue or its tests is started with `--ulimit memlock=-1:-1`, as `scripts/release_check.py`
does for the Rocky 8 container.

The machine must also have the memory to spare: two engines on one machine lock twice the figure between them, and a
sequencer on the same machine adds its own.

## Why the sequencer locks its memory

The leading sequencer checks every command against its record of command identifiers, a table of each command's
identifier and its position in the WAL, so that it writes a command into the WAL at most once
([one_check_against_sequencing_twice.md](../availability/one_check_against_sequencing_twice.md)). The table is reserved
in full when the sequencer starts, and every page is written as it is reserved, so the memory is there; but unlocked,
the kernel may swap pages of it out under memory pressure, and the thread that sequences orders would wait for one to
be read back from disk. So the table is locked as it is reserved. The design is in
[the_sequencer_holds_its_memory_from_the_start.md](../venue/the_sequencer_holds_its_memory_from_the_start.md).

Its size is set by `identifiers_reserved` in each environment file: 200 million identifiers in test-1, pre-production
and production, a table of 6,144 MB, and 20 million in development, 768 MB.

## What the sequencer logs

The table is built on a helper thread after the sequencer is ready, so that a restarted sequencer starts at once, and
it takes about two seconds at production's size. So the sequencer compares the figure with its limit first, as it
starts, at Info:

```
SequencerThread: it will lock 768 MB of memory, for the record of command identifiers, against a limit on locked memory of 3980 MB
```

and once the table is built and locked, about half a second later in development:

```
SequencerThread: the record of command identifiers is locked into memory, 768 MiB
```

If the limit is lower than the figure, the sequencer logs at
Critical and exits with status 1, without reserving anything:

```
SequencerThread: refusing to start -- it needs 768 MB of locked memory, and this process's limit on locked memory (RLIMIT_MEMLOCK) is 100 MB. ...
```

If the limit is high enough but locking still fails, because the machine does not have the memory free, the sequencer
logs at Critical that the record could not be locked and why, and stops with status 1 although it is already running.
If it leads, its peer takes over, as when a leader dies.
