# FIX Load Client

**Status: DRAFT, 2026-08-09. Nothing built. Not before 0.3.0.**

A load generator that speaks FIX, so the trading-day profile can drive the FIX order gateway as
well as the binary one, and the cost of FIX parsing can be measured on a normal day rather than
inferred from a profiler.

## The gap

`binary_load_client` drives the binary gateway. There is no counterpart for FIX, and two
consequences follow that are easy to miss.

**The trading-day profile is binary-only.** `perf_run.py`'s `run_profile_phase()` invokes
`binary_load_client` unconditionally. Every scenario in
[Compressed Trading Day Load Profile](trading_day_load.md) — the phases, the cancels, the
sustained hour, the memory findings — is unavailable over FIX.

**`--gateway fix` drives `f8test`, which is not ours.** It is external to this repository, and
it is already recorded in [Bug List](../bug_list.md) as dying on a Logon-level gap, which is why
the gateway's resend path has no end-to-end test. Owning the client removes an external
dependency that is known to fail on the case a load run is most likely to produce.

## Why this is worth building

**The comparison was designed for and cannot be run.** `environments/dev.toml` ranks only the
`_a` instance of each protocol for a dedicated core, and says why: the two measured gateways are
one rank group so they "can never be split across core types… what makes the FIX-versus-binary
comparison valid by construction rather than by arithmetic accident." The core-layout machinery
exists to make a comparison that has no FIX-side load generator.

**The charting end is already done.** `pubsub_metrics.py` synthesises
`compare:order_round_trip_nanoseconds` for any histogram exposed by more than one component, and
`--overlay` draws them on one axes. Only the load client is missing.

**What exists is not the measurement wanted.** A FIX-versus-binary comparison was run on
2026-08-04, 20,000 orders per gateway:

| | p50 | p99 | mean |
|---|---|---|---|
| fix_order_gateway_a | 171 ms | 248 ms | 146 ms |
| binary_order_gateway_a | 112 ms | 247 ms | 107 ms |

Those are **queueing figures, not service time**. `perf_run` without `--rate` saturates
deliberately, with `perf record --call-graph dwarf` attached throughout, so both p99s sit against
the same ~250 ms ceiling and only p50 and mean distinguish the protocols. They must not be quoted
as gateway latency.

The question worth answering is different: the same trading-day profile through both gateways,
paced below the ceiling, over millions of orders, with the tail meaning something. Callgrind puts
FIX parsing at roughly 8% of gateway CPU against the Quill backend's 27% and
`drain_pending_cancels` at 11.3%. Whether that holds end to end under sustained load, where
parsing competes with everything else and the tail is the point, is not the same question.

## Scope fence

The value of this work is entirely dependent on it staying a load client. A FIX **engine** is a
different, much larger project, and each item in the second list below is a route into it.

**In scope:**

- initiator only, one session per comp id
- Logon, Logout, Heartbeat, TestRequest — SCRAM and proprietary, since the gateway supports both
- outbound sequence numbers, and **surviving an inbound gap without dying**, which is the one
  thing `f8test` gets wrong
- NewOrderSingle and OrderCancelRequest encoded through `fix_codec`, driven by the data
  dictionary
- ExecutionReport decoded far enough to match ClOrdID and close the round trip
- rate pacing, phase support, cancel ratio, ClOrdID high-water marks, loss accounting, latency
  histogram

**Out of scope, and each of these turns this into a FIX engine:**

- message store, persistence, replay
- full ResendRequest recovery — *not dying* on a gap is required, *recovering* from one is not
- acceptor role
- repeating-group generality beyond what NewOrderSingle, OrderCancelRequest and ExecutionReport
  need
- any administrative message beyond the five named above

## Prerequisite: both clients must share the harness

**This is a correctness requirement for the comparison, not a tidiness preference.** If the two
clients pace with different clock discipline, or compute percentiles differently, or account for
losses differently, then a FIX-versus-binary chart compares *clients* rather than gateways — and
the core-layout work described above was done specifically so the comparison would be valid by
construction.

So the first step is not writing FIX. It is lifting the rate pacing, ClOrdID high-water handling,
loss accounting and latency histogram out of `BinaryLoadClientMain.cpp` into something both link
against. That is a refactor of code that already exists, already works, and has already survived
nine trading-day runs — the cheapest and lowest-risk part of the job. The FIX client then fills
in the protocol.

It also creates the seam that makes the harness reusable elsewhere: everything above the
protocol is venue-agnostic.

## C++, for measurement fidelity rather than throughput

The client timestamps the send and the ExecutionReport to compute the round trip, so **any pause
inside the client is indistinguishable from venue latency**. A garbage collection pause in the
load generator appears on the chart as a round trip of the same length.

Throughput is not the argument — a managed runtime would reach this profile's rates without
difficulty. The argument is that this entire body of work lives in the tail: a 2.5 ms ceiling,
p99.9 figures, multi-second stalls that turned out to be real defects. A client that can inject
its own pauses into that measurement makes every tail reading arguable. `binary_load_client` is
C++ for this reason and its counterpart should be.

## Build on `fix_codec`

`fix_codec` is already dictionary-driven, already has a `data_dictionary` directory, and the
gateway has been migrated onto it and live-verified. Most of the encoding work is done, and a new
third-party FIX library would be expensive here: the build may not reach the network, and
third-party artefacts must be Rocky8-built.

**Hard-code no tags.** Everything from the dictionary. Beyond being the house rule
([Demo DD is a FIX50SP2 subset](../../applications/fix_common)), it is what keeps the harness
portable to a venue with a different dictionary — which is the only part of this that transfers
anywhere.

**One caveat to cover deliberately.** Client and gateway would then share an encoder, so a
`fix_codec` encoding bug could be masked by its own matching decoder: a round trip would succeed
while both ends were wrong together. Round-trip success therefore does not prove correctness, and
the dictionary-level validator should be run against captured traffic rather than trusting the
loop.

## Sizing

`BinaryLoadClientMain.cpp` is 840 lines in one file, linking `pubsub_itc_fw`. The FIX equivalent
is larger — session layer, TLS, SCRAM logon, dictionary-driven encoding — but the estimate is
**1,200–1,800 lines, of which roughly 300 already exists** and would move into the shared harness.

The session layer is the hard part, and it is exactly where `f8test` fails.

## Open questions

- Does the FIX client need TLS from the start? The gateway's listener has it, and the certificates
  are deployed; a plaintext-only client would restrict which listener it can drive.
- Should phases restart the session, as `binary_load_client` does, or hold one session and vary
  the rate? Restarting is visible on the chart as a logon at each phase boundary. Holding the
  session would remove that artefact but needs in-flight rate changes, which the binary client
  cannot do either.
- Is one process with N sessions right, or N processes? The binary client takes `--clients`.

## See Also

- [Compressed Trading Day Load Profile](trading_day_load.md) — the profile this would let FIX run
- [FIX Codec](fix_codec.md) — the dictionary-driven codec to build on
- [Metrics](metrics.md) — `order_round_trip_nanoseconds` and the comparison view
- [CPU Core Layout](cpu_pinning_anti_affinity.md) — why both gateways share a rank group
