# Keeping WAL segments ready {#keeping_segments_ready}

**Status: built (BUG-0130 closed 9 November 2026).** This is the design for [BUG-0130](../bug_list.md#bug_0130). As
built, two details differ from the text below. The final name is given with `link()`, which fails if the name
exists, rather than `renameat2` with `RENAME_NOREPLACE`: the same effect, without depending on a newer call. And the
temporary names are `filling_wal_NNNNNN.helper` and `filling_wal_NNNNNN.writer`, not `wal_NNNNNN.filling`: three
readers (`WalReader`, `WalCursor` and `WalCommandSearch`) find segments with `sscanf("wal_%06" ... ".log")`, which
matches any name that merely begins like a segment's, and a temporary name beginning `wal_` was taken for the segment
itself, which two random replication tests showed by failing to open it. Section 3.6 missed those three.
It builds what was agreed with Andrew on 11 October 2026; the two additions in section 6 were agreed on 9
November 2026.

## 1. The problem

A WAL is a series of segment files, 4 MiB each in the sequencer's and the publishers' configurations.
Each new segment is filled with zeros when it is created, so that later writes into it allocate no disk
blocks ([BUG-0070](../bug_list.md#bug_0070)). Filling is the one step that can wait for the disk, so a
helper thread, `WalSegmentPrep`, fills the next segment while the writing thread appends to the current
one. It keeps one segment ready, no more.

When the current segment is full, `WalWriter::ensure_capacity` takes the ready segment if the helper has
finished it. If the helper is still filling it, the writing thread waits in `await_helper` until it
finishes. It cannot make the segment itself, because the helper is writing that very file, and mapping a
partly written file makes an ordinary append fail with SIGBUS. That wait breaks principle F2, since an
order-handling thread waits for another thread, and the thread it waits for is waiting for the disk, so
F4 is broken too. In a burst a 4 MiB segment can fill in about a tenth of a second, so a disk stall of a
few hundred milliseconds is enough.

If the helper failed, or was never started, the writing thread makes the segment itself, and nothing is
logged.

## 2. What was agreed on 11 October 2026

1. The helper keeps a configurable number of filled segments ready, four by default, so that the writer
   waits only if the disk stalls for longer than it takes to fill that many segments.
2. When the writer makes a segment itself because the helper has failed, it logs a Warning, so that it
   is treated as an incident and not only counted. The first segment at start-up is not a warning.

F4 names one accepted exception: a segment made by the writing thread because the helper has failed.
Waiting for the helper is not accepted.

## 3. The design

### 3.1 A segment's final name means it is full

Today a new segment is created under its final name, `wal_000123.log`, and filled in place. That is what
makes the writer wait: while the helper is filling `wal_000123.log`, the writer cannot create or map that
file.

**The change.** Whichever thread fills a segment fills it under a temporary name, `wal_000123.filling`,
syncs it, and renames it to `wal_000123.log` only when it is full. The rename is made with
`renameat2(..., RENAME_NOREPLACE)`, which fails if the final name already exists. So a file named
`wal_NNNNNN.log` is always complete, and of the two threads, whichever renames first wins:

- if the writer finds `wal_000123.log` already there, the helper finished it; the writer opens it and
  uses it;
- if the helper's rename fails because the name is taken, the writer got there first; the helper deletes
  its temporary file and moves on to the next segment.

Neither thread ever waits for the other, and neither ever maps a file that is still being filled. The
code that finds segments, `Wal::segment_numbers`, matches only names of the form `wal_NNNNNN.log`, so it
never sees a temporary file.

**This also closes a hazard found while designing this.** Today, if the process dies while a segment is
being filled, a short file is left under its final name. On the next start the writer opens any existing
segment file as one already prepared and maps the full 4 MiB of it, and an append past the end of the
short file would fail with SIGBUS. That was found by reading `WalWriter::create_and_map`; it has not been
reproduced, and step 1 of section 5 reproduces it before it is fixed. With the rename, a file left
behind by such a death has the temporary name and is deleted at the next start (section 3.4).

### 3.2 Several segments ready

The helper keeps up to `segments_kept_ready` segments ready ahead of the one being written, four by
default. They are held in a small fixed array of slots, one for each ready segment, with the segment for
number *n* in slot *n* modulo the number of slots. Two counters say how far each thread has got, each
written by one thread only, in the same way as `BackgroundPromiseRecorder`:

- **the helper's count** is the highest segment it has made ready and published in its slot;
- **the writer's count** is the segment it is writing.

The helper prepares the next segment whenever it is fewer than `segments_kept_ready` ahead of the
writer. A slot belongs to the helper until the helper publishes it, and to the writer after that. No
lock is taken.

When the current segment is full, the writer looks in the slot for the next segment:

- **It is ready**, which is the normal case: the writer takes it, records the new segment number, and
  wakes the helper to prepare another. Nothing on this path opens, fills or maps a file.
- **It is not ready**, because the disk has stalled for longer than it takes to fill
  `segments_kept_ready` segments, or the helper has failed: the writer makes the segment itself, by the
  rule in 3.1, and logs a Warning (3.3). It does not wait for the helper.

A published slot for a segment the writer has already passed, because the writer made that segment
itself, is released by the writer at its next roll.

### 3.3 The Warning

When the writing thread makes a segment itself, other than the first at start-up, it logs a Warning
naming the WAL, the segment, how long the fill took, and how many segments were ready (none, by
definition). The count `segments_filled_inline` stays, for the metrics. The count
`segments_waited_for` is removed, because the writer no longer waits.

### 3.4 At start-up

When the writer opens a WAL:

1. It deletes every temporary `.filling` file in the directory. Each is a segment whose fill was cut
   short, and none holds a record.
2. It checks the size of each existing segment from the one it resumes in onwards. One shorter than the
   segment size can only have been left by a version of the writer that filled segments in place. The
   writer fills the rest of it with zeros, which keeps any records in it, and logs a Warning.
3. Segments already ready from earlier in the same day are used as they are; they are complete by 3.1.
4. The helper then fills the slots up to `segments_kept_ready` ahead.

### 3.5 The setting

`segments_kept_ready` in the `[wal]` section of the sequencers' and the publishers' configurations, 4
by default, at least 1. Each ready segment costs one segment of disk, 16 MiB in all at the default, and a
mapping of the same size, which is address space rather than memory.

### 3.6 What else reads the segment files

More than one empty segment can now lie beyond the last record. Every reader was checked for an
assumption that the newest file holds records. Each already copes with one empty segment beyond the last
record, which the single ready segment leaves today, and none depends on how many there are:

- `WalCommandSearch::find_newest_segment_with_records` walks back from the newest file until it finds
  one whose first record marker is present;
- `Wal::first_seq_no` and `Wal::first_wall_time_ns` read forwards from the oldest;
- the search in `Wal` for the segment holding a sequence number searches only up to the segment being
  written, and treats a segment with no first record as coming after every record;
- `WalCursor` and `WalReader` stop at the first position whose record marker is missing;
- `Wal::truncate_after` deletes every segment after the one it cuts, ready ones included, after closing
  the writer, which stops the helper.

## 4. The principles this restores

F2: no order-handling thread waits for another thread. The writer never waits for the helper.

F4: no order-handling thread waits for the disk, apart from the accepted exception. The writer fills a
segment itself only when `segments_kept_ready` segments have all been used up while the helper could not
make another, and that is logged as a Warning.

## 5. Steps, and how each is known to be right

1. **The hazard in 3.1 reproduced**, in a unit test that leaves a short `wal_NNNNNN.log` and opens the
   WAL. Against the writer as it is now, an append past the end of the short file must fail with SIGBUS,
   run as a test that expects the process to die. Against the new writer it must succeed and keep any
   records already in the file.
2. **Several segments ready.** Unit tests: after opening, `segments_kept_ready` complete segments exist
   ahead of the current one; rolls through many segments take ready ones and make none inline; a temporary
   file left in the directory is deleted at opening.
3. **The writer does not wait.** A test-only delay in the helper's fill, longer than it takes the writer to
   use up every ready segment. The roll must take a fraction of the delay, a segment must be made inline,
   and the Warning must be logged once for it. Made to fail on purpose first by putting the old wait back.
4. **The race in 3.1.** The helper's rename made to lose to the writer, by the same test delay. The helper
   must delete its temporary file, no file may be mapped while being filled, and the records must read
   back intact.
5. **ThreadSanitizer** over the WalWriter tests, which found the last race in this class.
6. **The full high-availability suite and a trading-day run**, with the WALs cleared first. In a
   trading-day run the counts should be one segment made inline, the first, and no Warnings.

## 6. What is new beyond what was agreed

Two things, both agreed on 9 November 2026:

- **The rename in 3.1.** Without it the writer must either wait for the helper or risk mapping a file the
  helper is still filling, so it is what makes "never wait for the helper" possible. It also closes the
  start-up hazard described there.
- **The setting's name**, `segments_kept_ready`, in `[wal]`.
