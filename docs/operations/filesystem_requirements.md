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

```
mount -o remount,lazytime <the filesystem holding the log>
```

To survive a reboot, add `lazytime` to that filesystem's options in `/etc/fstab`.

The sequencer checks this at startup and says what it found. If the option is missing it logs a
warning naming the directory and the options actually in force. It is a warning rather than a
refusal: the venue is correct without `lazytime`, only slower in the tail.

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
