# Metrics and Prometheus {#metrics}

How this project exposes measurements, and the decisions behind the shape of it.

> **Status: partly built.** `MetricKey`, the metric interfaces and their no-op and
> prometheus-cpp implementations exist. `PrometheusEndpoint` registers metrics but does not
> yet serve them -- there is no `Exposer` yet -- and nothing is instrumented. The
> configuration described under [Configuration](#configuration) is agreed but not
> implemented. Sections marked **open** are undecided.

---

## Why pull, and why prometheus-cpp

Prometheus scrapes; it is not sent to. Each process therefore runs a small HTTP listener
that renders its current metrics on demand. This suits every component here, because they
are all long-running processes that can be scraped in place.

The alternative in the same library is the *pushgateway* client, which exists for jobs too
short-lived to be scraped before they exit. Nothing here is like that, so the push library
is deliberately not linked -- it would only pull in libcurl for a path never taken.

The client library is **prometheus-cpp 1.3.0**, wired in as an ordinary third-party
dependency (`PROMETHEUS_VERSION`); see the top-level `CMakeLists.txt`. Not to be confused
with Prometheus itself, which is a Go server and is not vendored here.

---

## Naming: `MetricKey`

A metric is identified by a **metric key**, written in a dotted form:

    <application>.<component>[.<scope>].<metricName>

Three tokens, or four when a scope is present. `MetricKey` parses and validates it, and
splits it into the parts Prometheus wants:

| Token    | Becomes                |
|----------|------------------------|
| first    | `application` label    |
| second   | `component` label      |
| third    | `scope` label (if any) |
| last     | the metric name        |

So `pubsub.gateway.binary.socket_latency_seconds` yields:

    socket_latency_seconds{application="pubsub", component="gateway", scope="binary"}

**The metric key is never given to Prometheus in its dotted form.** Only the parts are. This is why the
grammar can use dots freely as separators without needing to escape anything.

### Validation rules

Checked in the constructor, which throws `ConfigurationException` -- metric keys come from
configuration, so a malformed one is a deployment error rather than a programming one.
Failing at configuration-load time means the operator gets a message naming the key and
the fault, instead of a metric quietly missing from a dashboard weeks later.

- three or four tokens, no more and no fewer;
- every token is one or more of `[A-Za-z0-9_]`, so no token may be empty -- which also
  rejects a leading dot, a trailing dot and a doubled dot;
- the **metric name may not start with a digit**, because Prometheus requires metric names
  to match `[a-zA-Z_:][a-zA-Z0-9_:]*`. The other tokens become label *values*, which have no
  such restriction, so a component or scope of `5xx` is fine while a metric name of
  `5xx_total` is not.

Because the token count is capped at four, a scope containing a dot cannot be expressed --
it simply reads as too many tokens, and the error message says so. That is the intended
design: scope is one token.

Colons are excluded even though Prometheus permits them in metric names, because Prometheus
reserves them by convention for recording rules.

`MetricKeyTest.cpp` covers the grammar exhaustively, including the cases where the metric
name is stricter than the other tokens.

---

## Metric types and the on/off switch

Three interfaces -- `CounterInterface`, `GaugeInterface`, `HistogramInterface` -- each with
a prometheus-cpp implementation and a no-op one. `PrometheusEndpoint::register_*` returns a
reference to the interface, so **no call site knows which it got**.

That is what makes metrics switchable at one point. When metrics are disabled the endpoint
returns references to shared no-op instances; the no-ops are stateless, so one instance of
each type serves the whole process.

Two consequences worth knowing at call sites:

- an observation on a disabled metric still costs a virtual call, because an empty virtual
  function cannot be inlined away. That is a few nanoseconds and is not worth avoiding;
- **the argument is still evaluated.** `observe(expensive())` pays for `expensive()` whether
  or not metrics are on. Where computing the value is itself costly, guard the computation
  rather than relying on the no-op.

The interfaces are named for what they are, not for Prometheus, so a second backend would
not make every call site a lie.

---

## Configuration {#configuration}

A `[metrics]` section per component:

```toml
[metrics]
enabled     = ${shared_metrics_enabled}
listen_host = "${<component>_metrics_listen_host}"
listen_port = ${<component>_metrics_listen_port}
```

`enabled` comes from a shared placeholder so metrics can be turned on or off venue-wide in
one edit, exactly as `reactor_cpu_pinning_reserve_cpu0` already is. The endpoint is
per-process, so **host and port are per component**: roughly fifteen processes run on one
host in dev and they cannot share a port.

`listen_port = 0` binds an ephemeral port. This is deliberately supported: it lets tests run
in parallel without colliding on a fixed port, and `Exposer::GetListeningPorts()` reports
what was actually bound. The endpoint should log the real port at startup -- with fifteen
scrape ports in play, reading it from the log beats deriving it from placeholder expansion.

Validation: when `enabled` is true, the host must be non-empty. When it is false both are
ignored, because a disabled endpoint should not fail on a port nobody will bind.

### Which host to bind

Dev binds `127.0.0.1`; preprod, prod and test-1 bind `0.0.0.0`. This is not an oversight in
either direction:

- `0.0.0.0` is required where Prometheus scrapes from another host, which is the normal
  production topology. `127.0.0.1` would be unreachable.
- `127.0.0.1` is right in dev, where fifteen processes on a workstation would otherwise each
  expose an unauthenticated port to the local network, and right anywhere the scrape is done
  by a local agent or sidecar.

**The endpoint is unauthenticated and unencrypted.** civetweb is built here with
`PROMETHEUS_CPP_THIRDPARTY_CIVETWEB_WITH_SSL OFF`, so whatever can reach the port gets the
whole metric set -- order counts, per-component latencies, error rates, session counts. That
is commercially meaningful for a venue, not merely operational. Which interfaces it is
exposed on is therefore a security decision per environment, and it belongs with the wider
open question about encrypting internal traffic in
[Secure Communications](secure_comms.md).

---

## Threading

### The scrape threads

`Exposer` starts civetweb worker threads (two by default). These must not land on hot-path
cores.

They inherit the affinity of whichever thread creates them, and **ordering alone does not
place them correctly** -- which was established by measurement, after an earlier version of
this document claimed otherwise.

`CpuLayout` masks the whole process to the background cores and *then* pins individual
hot-path threads to their own cores. `Reactor::run()` starts the listener after that, by
which point the calling thread is itself pinned to a hot-path core -- so the listener
threads inherited it. All three civetweb threads of `fix_order_gateway_a` were found on core
3, shared with that gateway's reactor thread: precisely the core the layout exists to keep
clear.

`Reactor::start_metrics_endpoint()` therefore widens its own affinity to the background
cores for the duration of the call and restores it immediately afterwards. Verified on a
running venue: the civetweb threads now report `15-31` while the reactor thread stays on
core 3 and the application thread on core 4.

This is also the reason **construction is separate from starting**. The caller needs a point
at which it can arrange the affinity, and the background core list is not known until the
layout has been read -- so a constructor could not do it.

Worth watching: two civetweb threads per process across fifteen processes is thirty threads
sharing the background tier. `Exposer` takes a thread count; one may well be enough.

### Metric objects and locking

- `Counter` and `Gauge` use `std::atomic<double>`.
- **`Histogram::Observe` takes a mutex.** This is not removable by giving each thread its own
  metric, because the *scrape thread reads the same object*: the lock protects recorder
  against collector, not recorder against recorder.

Uncontended that is tens of nanoseconds, and it contends only during a scrape. If a
measurement ever shows it mattering on the order path, the established escape is
thread-local accumulation merged at collection time -- but that is real work and should wait
for evidence.

---

## Ownership and lifetime

The Reactor owns the endpoint. Every reference handed out by `register_*` points into the
registry the endpoint owns, so **the endpoint must outlive every holder** -- constructed
early, destroyed last.

The registry owns the metric families and the families own their children, so the destructor
needs no manual teardown. One ordering rule does matter: the `Exposer` must be destroyed
before the registry it collects from, or a scrape in flight can touch freed memory.
Declaring `exposer_` last achieves that, since members destruct in reverse declaration
order. It looks like arbitrary field ordering otherwise, so it is worth a comment.

---

## Registration rules

A metric family is keyed on the metric *name* alone. Two keys sharing a name is the normal
case, not an error:

    pubsub.gateway.fix.orders_total
    pubsub.gateway.binary.orders_total

One family, two children, distinguished by their labels. That is what the labels are for.

Prometheus allows one help string per family, so a second registration of the same name with
*different* help silently keeps the first. Help text comes from code rather than
configuration, so that is a programming error and should raise `PreconditionAssertion`
naming both strings, rather than being ignored.

---

## What a scrape returns

Prometheus fetches a plain-text body over HTTP, which its documentation calls the
*exposition format*. It is line-based, with two comment lines introducing each family:

    # HELP orders_total Orders accepted
    # TYPE orders_total counter
    orders_total{application="pubsub",component="gateway",scope="fix"} 12
    orders_total{application="pubsub",component="gateway",scope="binary"} 7

`HELP` carries the description, `TYPE` says counter, gauge or histogram, and each remaining
line is one time series: a metric name, its labels, and its current value. The two lines
above are one family with two children, which is what a shared metric name with differing
scopes produces.

A histogram renders as several lines per series -- one `_bucket` line per boundary, then
`_sum` and `_count`:

    latency_seconds_bucket{component="gateway",le="0.001"} 3
    latency_seconds_bucket{component="gateway",le="+Inf"} 5
    latency_seconds_sum{component="gateway"} 0.42
    latency_seconds_count{component="gateway"} 5

`PrometheusEndpoint::exposition_text()` returns exactly this, which is what the tests assert
against.

---

## Testing

`PrometheusEndpoint` is testable directly, without a fake registry. `Registry` is a
`Collectable`, and prometheus-cpp ships a text serializer, so a test can register metrics and
assert on the real exposition output -- names, label sets, help text, bucket boundaries -- and
that a disabled endpoint produces nothing at all. That is a stronger check than a fake would
give, because it tests the mapping the design is actually about.

Binding port 0 keeps such tests free of fixed-port collisions.

An abstraction over the endpoint itself would only be needed to test *call sites* without a
real registry. It is deliberately not there yet: the per-metric interfaces already provide
that seam if it turns out to be wanted.

---

## Open

- **What to measure.** Nothing is instrumented yet. The first purpose is comparing FIX
  against binary order entry, which requires instrumenting both identically -- a metric on
  one path and not the other would measure the instrumentation.
- **Bucket boundaries** for latency histograms.
- **Scrape interval and retention**, and whether a Prometheus server is deployed alongside
  the venue or scrapes from outside it.
- **Whether the endpoint should be exposed at all in preprod and prod**, pending the wider
  discussion about internal traffic in [Secure Communications](secure_comms.md).
