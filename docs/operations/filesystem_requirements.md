# Filesystem requirements for the write-ahead log

> **The filesystem holding the sequencer's log must be mounted `lazytime`.**
>
> Without it the sequencer stalls for hundreds of milliseconds at a time, on the thread that
> sequences every order the venue takes. With it, those stalls do not happen at all.
>
> This is a mount option. It is not in this repository, it is not in any configuration file the
> venue reads, and nothing about the code suggests it matters.

## The measurement

Two runs, twenty minutes each, on the same machine, under the same load profile
(`profiles/stall_hunt.toml`, `--gateway binary --clients 4`), against the same binaries. The log
was on its own device in both. **The only difference was the mount option.**

| | `relatime` | `lazytime` |
|---|---|---|
| Records committed | 9,362,122 | 9,294,671 |
| Appends over 1 ms | 24 | **8** |
| Appends over 10 ms | 15 | **0** |
| Appends over 100 ms | 4 | **0** |
| Appends over 200 ms | 3 | **0** |
| Appends over 500 ms | 1 | **0** |
| Appends over 1 s | 1 | **0** |
| Worst reactor stall | 845 ms | **under 1 ms** |
| Time in uninterruptible sleep | 0.100% | **0.004%** |

Not one append in 9.3 million exceeded 10 milliseconds with `lazytime` set.

## What `lazytime` is

A Linux mount option, understood by ext4 among others. To explain what it changes, first what
happens without it.

Every file carries three timestamps in its inode: when it was last read (`atime`), when its
contents last changed (`mtime`), and when the inode itself last changed (`ctime`). Writing to a
file updates `mtime` and `ctime`.

The inode is not the file's data. It is **filesystem metadata**, and on a journalling filesystem
every metadata change goes through the journal, so that a crash cannot leave the filesystem's own
bookkeeping half-written. That is what a journal is for, and it is a good thing: it is why an
ext4 filesystem comes back consistent after a power cut.

The cost is that a metadata change is not a free write to memory. It must be recorded in a
journal transaction, transactions commit periodically, and a thread that needs journal access
while a commit is in progress **waits** — in uninterruptible sleep, which cannot be interrupted
or preempted.

`lazytime` changes only this: timestamp updates are kept **in memory** and written to disk lazily
— when the inode is written for some other reason, when the file is closed, or after at most 24
hours. The timestamps are still correct to anything asking the running kernel. What is avoided is
a journal transaction *per writeback* purely to record that a file's mtime moved.

The trade is small and worth stating: if the machine loses power, timestamps may be up to a day
stale on recovery. File **contents** are unaffected — `lazytime` changes nothing about data
integrity, only about when the clock fields reach the disk. For a trading log whose records carry
their own timestamps inside them, the file's mtime is of no consequence.

## How it was arrived at

Not by guesswork, and not first. The order matters, because three earlier ideas were wrong or
unnecessary.

**1. The stall was located before it was explained.** `scripts/thread_offcpu.py` samples
`/proc/<pid>/task/<tid>/stat`, `schedstat` and `wchan` a few hundred times a second. It showed the
sequencer's thread in state **D** — uninterruptible sleep — with no time on the run queue. That
alone ruled out lock contention and cpu starvation: a contended mutex produces state S, and a
thread waiting for a cpu accumulates run-queue time. Neither was happening.

**2. `wchan` named the mechanism.** For a thread in state D that file gives the kernel function
it is sleeping in. The answers were `do_get_write_access` and `wait_transaction_locked` — asking
the ext4 journal for permission to modify a metadata block, and waiting for a journal transaction
to commit. So the stall was the journal, established rather than suspected.

**3. The first explanation was right but incomplete.** Segments were created with `ftruncate`,
which leaves a file **sparse**: its size is set and not one block is allocated. The first write
to each page then has to allocate a block, and allocation is a metadata change. That is a real
cause, it was fixed (see [BUG-0070](../bug_list.md#bug_0070)), and the common tail improved by
more than ten times — but the large stalls remained. **Block allocation was the suspected cause
and it was not the whole answer.**

**4. What was left had to be metadata that was not block allocation.** With the blocks already
allocated, the remaining journal traffic could only come from something else the writes were
still changing. The inode's timestamps are the obvious remaining candidate: appending is a
`memcpy` into a mapping, so every writeback of a dirty page updates `mtime`, and each such update
is a metadata change. `lazytime` is the option that stops exactly that.

**5. It was tested against the alternative, one change at a time.** The log was first moved to
its own device, changing nothing else — that isolated block-layer contention and accounted for
249 samples. Then `lazytime` was added, changing nothing else. `wait_transaction_locked` went to
zero and `do_get_write_access` fell eighteen-fold, which is the confirmation: the remaining
journal traffic really was timestamps.

A fourth idea, calling `sync_file_range()` from a helper thread to control when writeback
happened, was planned as the next experiment and became unnecessary — there was nothing left for
it to fix. It was never built.

## Why it makes that much difference

Appending to the log is a `memcpy` into a memory-mapped file. There is no `write`, no `fsync`
and no `msync` anywhere on that path, so nothing about the source code suggests a disk is
involved at all.

What happens underneath:

1. The `memcpy` dirties a page of the mapping.
2. When the kernel later writes that page out, it also updates the file's **inode timestamps**.
3. An inode change is **filesystem metadata**, and metadata changes go through the ext4 journal.
4. A thread waiting for a journal transaction to commit is in **uninterruptible sleep** — it
   cannot be interrupted or preempted, and it is not on any run queue.

So an ordinary memory copy is occasionally a journal operation, and the wait lands on whichever
thread happened to be appending. `lazytime` keeps timestamp updates in memory and flushes them
periodically instead of on every writeback, which removes step 3 for almost all of them.

Measured directly with `scripts/thread_offcpu.py`, which reports the kernel function a stalled
thread is waiting in:

| Kernel function | `relatime` | `lazytime` |
|---|---|---|
| `do_get_write_access` (asking the journal for permission to change metadata) | 364 | **20** |
| `wait_transaction_locked` (waiting for a journal transaction to commit) | 165 | **0** |

The journal traffic was almost entirely timestamps. That was not the expected answer — block
allocation was the suspected cause, and it was not.

## What to do

**This must be run as root.** It takes effect immediately and is not destructive: no data is
moved, no filesystem is unmounted, and processes with files open are unaffected.

```
# as root
mount -o remount,lazytime /mnt/sda2          # the filesystem holding the log
findmnt -no SOURCE,OPTIONS /mnt/sda2         # confirm: expect rw,lazytime,relatime
```

A remount does **not** survive a reboot. To make it permanent, add `lazytime` to that
filesystem's options in `/etc/fstab`:

```
/dev/sda2  /mnt/sda2  ext4  defaults,lazytime  0  2
```

then `mount -o remount /mnt/sda2` to apply it, or verify at the next boot with `findmnt`.

The sequencer checks this at startup and says what it found. If the option is missing it logs a
warning naming the directory and the options actually in force. It is a warning rather than a
refusal: the venue is correct without `lazytime`, only slower in the tail.

## Telling the venue which device to use

The device is named by the environment, not by a configuration file:

```
export PUBSUB_WAL_ROOT=/mnt/sda2/mystuff2      # before scripts/devsetup.sh
```

`deploy.py` then places every write-ahead log under it, and says so:

```
PUBSUB_WAL_ROOT is set: write-ahead logs go under /mnt/sda2/mystuff2
```

Unset, the logs go under the install directory as before, which works anywhere. If it is set to
something that is not a directory, the deploy stops rather than carrying on.

**Why this is not simply written in `dev.toml`.** That file serves *both* development
environments -- the Linux Mint host and the Rocky/RHEL8 container -- which differ by platform,
not by environment file. An absolute path in it would exist on one and not the other, and
`deploy.py` creates the directory it is given, so the container would quietly get a log on
whatever filesystem it happened to have. The environment file says the log wants a directory;
the machine says where its disk is.

## A second, smaller finding: give the log its own device

Before the `lazytime` run, moving the log off the shared device was measured on its own. It is
worth doing but it is not the main effect.

Everything had been sharing one disk: the log, the matching engine's 496 MB open-order region,
the application logs, and gigabytes of `perf` captures. The sequencer's commits were queueing
behind writes that had nothing to do with trading.

| | shared device | own device |
|---|---|---|
| `rq_qos_wait` (block layer making the thread wait) | 249 | **4** |

That change removed the block-layer contention and left the journal waits untouched, which is
what made the `lazytime` result readable when it came.

## How this was established, and why it took two runs

Each run changed **one thing**. Run A moved the log to its own device and changed nothing else;
run B added `lazytime` and changed nothing else. That is what allows each number above to be
attributed to a specific cause.

Had both been changed at once, the result would have been a single good figure and no way to
tell which change earned it — and no way to know that `sync_file_range`, which was the next
planned experiment, had become unnecessary. It had: with `lazytime` there was nothing left for
it to fix, so it was never built.

## Applies to more than the log

Any memory-mapped file the venue writes to has the same exposure, because the mechanism is
about mapped writeback rather than about the log. The matching engine's open-order region at
`installed/var/matching_engine_open_orders.region` is the other one, and it is not yet on a
`lazytime` filesystem. See [BUG-0071](../bug_list.md#bug_0071), which records a related defect
in how that region is warmed.

## Related

- [BUG-0070](../bug_list.md#bug_0070) — the original stall, how it was found, and the code change
  that preceded this
- [BUG-0071](../bug_list.md#bug_0071) — the open-order region is created sparse and warming it
  reads rather than writes, so it allocates nothing
- `scripts/thread_offcpu.py` — the probe that identifies which kernel function a stalled thread
  is waiting in, without source, symbols, or privileges
