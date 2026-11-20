# The matching engine's limit on locked memory {#locked_memory_requirement}

> **One rule.** The user the venue runs as must be allowed to lock at least as much memory as each matching engine
> locks: its order book's map, its open-order region and, with high availability on, the store of reports it keeps
> while following the WAL, about 65 GB in production. Each process has its own
> limit, so the figure is per engine, not for the pair. Without it, the engine refuses to start.

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

The machine must also have the memory to spare: two engines on one machine lock twice the figure between them.
