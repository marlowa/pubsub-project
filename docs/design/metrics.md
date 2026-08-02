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

The Reactor owns the endpoint. Every handle handed out by `register_*` points into the
registry the endpoint owns, so **the endpoint must outlive every holder** -- constructed
early, destroyed last.

The registry owns the metric families and the families own their children, so the destructor
needs no manual teardown. One ordering rule does matter: the `Exposer` must be destroyed
before the registry it collects from, or a scrape in flight can touch freed memory.
Declaring `exposer_` last achieves that, since members destruct in reverse declaration
order. It looks like arbitrary field ordering otherwise, so it is worth a comment.

---

## Recording handles

`register_counter`, `register_gauge` and `register_histogram` return a **`CounterHandle`,
`GaugeHandle` or `HistogramHandle` by value**, not a reference to the interface.

A handle is not a `CounterInterface`. It does not derive from one and takes no part in the
hierarchy; it holds a pointer to one. `PrometheusCounter` and `NoOpCounter` remain the
implementations and the virtual call still happens. The handle exists purely so callers have
something they can hold by value.

Returning a reference was the obvious first shape, and it was wrong for reasons that all bite
at the call site:

- A reference member must be initialised in the constructor's initialiser list, which runs in
  member *declaration* order. A metric whose scope is built from another member then depends
  on the two being declared in the right order, and getting it wrong captures an empty string
  silently rather than failing to compile. A value member is assigned in the constructor body
  and the ordering trap disappears.
- A reference member makes its enclosing class non-assignable -- a lasting restriction to have
  acquired from the decision to count something.
- A default-constructed handle is a safe no-op, so a class can hold one unconditionally and
  register only on the paths that need it. This is what makes the opt-in scope below possible.

What handles do **not** provide is lifetime safety: the pointer dangles if the endpoint is
destroyed first, exactly as a reference would. That is acceptable because the endpoint is a
Reactor member and outlives every registrant by construction, and because registrations live
in node-based `std::map`s whose elements do not move as further metrics are registered.
`PrometheusEndpointTest.HandlesStayValidAsMoreMetricsAreRegistered` pins that down.

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

## Who supplies which token

A key is `<application>.<component>[.<scope>].<metricName>`, and the four tokens have quite
different origins:

| token | source |
|---|---|
| `application` | `MetricsConfiguration.application` -- process-wide configuration |
| `component` | `MetricsConfiguration.component` -- process-wide configuration |
| `scope` | the registering object |
| `metricName` | a literal at the call site |

**A caller never supplies the application or the component.** The three-argument
`register_counter(scope, metric_name, help)` composes them from configuration, which is why it
exists: framework code declares the same metric in every component, so it cannot name any one
of them and a literal key would be wrong in nearly every process.

### The scope is named separately from the thread

`ApplicationThreadConfiguration::metrics_scope` carries the scope for a thread's metrics. It is
deliberately **not** derived from the thread name:

- Thread names are chosen for people reading logs. Renaming one would silently break every
  dashboard and alert built on it.
- They are CamelCase (`SequencerThread`) where the application and component label values are
  lowercase.
- They are not always legal metric tokens. The topic probe names its thread
  `"ProbeThread-" + topic_name`, and a hyphen is not permitted -- using the thread name
  verbatim would throw `ConfigurationException` and the probe would not start.

**An empty scope means the thread registers nothing**, and its handles stay unbound so
recording is a safe no-op. That is the default, and it is what makes the feature opt-in rather
than mandatory: two threads in one process both leaving the scope empty would compose the same
key, and registering a key twice is an error. Making it opt-in means the many test processes
that construct several threads against one Reactor need no changes, while a component that
wants the metric names its threads:

```cpp
ApplicationThreadConfiguration{ .metrics_scope = "sequencer_thread" }
```

The alternative defaults were considered and rejected: registering unconditionally with an
empty scope breaks every multi-threaded process at startup, and defaulting to the thread id
yields `scope="3"`, which never becomes meaningful.

---

## Instrumentation

### `framework_pdu_messages_total`

The first metric, and the pattern for those that follow. `ApplicationThread` registers a
counter when its configuration names a scope, and increments it in the `EventType::FrameworkPdu`
branch of its dispatch switch, immediately before `on_framework_pdu_message`.

    framework_pdu_messages_total{application="pubsub",component="sequencer",scope="sequencer_thread"}

It counts PDUs *delivered to the application*, not PDUs parsed off the wire -- the two differ
if delivery ever drops one, and the delivered count is what an application author reasons
about.

One series per thread, never one per process. A component that grows a second thread gets a
second series automatically, with no shared counter and no contention; the process total is a
`sum()` at query time. Aggregating in the exporter would throw away the breakdown and could not
be recovered later.

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

- **No component sets `metrics_scope` yet**, so `framework_pdu_messages_total` is registered
  by nothing and no series is exposed. Opting each component in is the immediate next step.
- **What else to measure.** The first purpose is comparing FIX against binary order entry,
  which requires instrumenting both identically -- a metric on one path and not the other
  would measure the instrumentation. The gateway-internal segments are where the two protocols
  actually differ; everything downstream is common to both.
- **Bucket boundaries** for latency histograms.
- **Scrape interval and retention**, and whether a Prometheus server is deployed alongside
  the venue or scrapes from outside it.
- **Whether the endpoint should be exposed at all in preprod and prod**, pending the wider
  discussion about internal traffic in [Secure Communications](secure_comms.md).
