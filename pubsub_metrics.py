#!/usr/bin/env python3
"""Visualise this venue's Prometheus metrics, especially the latency histograms.

Fetch and render can be done in one go, or split across machines: acquire on the
box running the venue, render wherever there is a display.

  1. Fetch and view in one step:

         python3 pubsub_metrics.py --component fix_order_gateway_a --graphic

  2. Compare every order gateway on one figure -- the question
     order_round_trip_nanoseconds exists to answer:

         python3 pubsub_metrics.py --component gateways --graphic

  3. Fetch on the venue host, save, and render elsewhere:

         python3 pubsub_metrics.py --component gateways --output gateways.dash
         python3 pubsub_metrics.py --input gateways.dash --graphic

Three data sources, resolved by the CLI:

  * live Prometheus fetch   (default; needs --component)
  * --input FILE            (replay a saved snapshot; needs --graphic)
  * --demo                  (synthetic data, no Prometheus; with --component,
                            synthesises that component's configured series)

Architecture -- three concerns kept physically separate, with LAZY imports so
each environment only needs what it has:

  * ACQUISITION   produces a DashboardData. Live fetch imports 'requests' only
                  when it runs, so a display-only machine never needs it.
  * SERIALIZATION writes/reads the replayable snapshot file. Pure stdlib.
  * RENDERING     displays a DashboardData. Imports matplotlib/tkinter only when
                  it runs, so the headless fetch+write side never needs them.

Rules:
  - at least one of --graphic or --output is required
  - --input and --output are mutually exclusive
  - --input requires --graphic
  - --component is required when fetching live (no --input, no --demo)

Prometheus is started with the venue by devenv.py; see docs/design/metrics.md.
"""

import argparse
import sys


PROM_URL = "http://localhost:9090"

# Vertical space one line of the monospace summary tables occupies, in inches. Ten-point
# text on a ~13-point line is about 0.18in; the margin above covers the descender of the
# last line so it cannot touch the plot beneath it.
TABLE_LINE_HEIGHT_INCHES = 0.20


# =========================================================================== #
# DATA MODEL
# =========================================================================== #
#
# The single value object exchanged between the three layers. Populated only
# through the builder functions, so every producer -- live fetch, file replay,
# synthetic demo -- travels one code path. Bounds and counts are numeric
# (float or int); real Prometheus values are floats.

class DashboardData:
    """Container for the three kinds of series shown on the dashboard."""

    def __init__(self):
        self.component = "sample"  # label shown in the figure title
        self.histograms = []       # list of {"name", "bounds", "counts"}
        self.counters = []         # list of (name, total, rate)
        self.gauges = []           # list of (name, value)


def set_component(data, name):
    """Set the component label carried by ``data``."""
    data.component = name


def add_histogram(data, name, bounds, counts):
    """Append one latency histogram to ``data``, ordered by bucket bound.

    A Prometheus scrape can return buckets in arbitrary order, so we sort the
    (bound, count) pairs here -- the single choke point every histogram passes
    through, whether fetched, replayed or synthetic.
    """
    ordered = sorted(zip(bounds, counts), key=lambda pair: pair[0])
    data.histograms.append(
        {
            "name": name,
            "bounds": [bound for bound, _ in ordered],
            "counts": [count for _, count in ordered],
        }
    )


def add_counter(data, name, total, rate):
    """Append one counter, carrying both its absolute total and its rate/s."""
    data.counters.append((name, total, rate))


def add_gauge(data, name, value):
    """Append one gauge (int or float) to ``data``."""
    data.gauges.append((name, value))


# =========================================================================== #
# ACQUISITION  (produces DashboardData)
# =========================================================================== #
#
# The only layer that knows where data comes from. Live fetch, file replay and
# synthetic demo all return the same DashboardData shape.

# --- Component configuration: maps each component to its Prometheus series -- #
#
# Every series this venue exposes is identified by application + component + scope.
# 'application' is the same everywhere, 'component' is the process INSTANCE
# (fix_order_gateway_a, not fix_order_gateway), and 'scope' names the thread within
# it. See docs/design/metrics.md.
#
# Only components that set a metrics_scope expose series of their own; the rest
# serve nothing but the exposer's own counters and are deliberately absent here.
#
# A histogram entry may carry its own 'labels', overriding the component's. That is
# what the "gateways" comparison view below uses to draw all four order gateways on
# one figure -- the whole reason order_round_trip_nanoseconds is a single metric
# distinguished by label rather than one metric per protocol.

APPLICATION = "pubsub"

# The gateways are identical in what they expose, so their entries are generated
# rather than repeated four times. Written out, the differences would be one word
# each and the temptation to edit only one of them would be real.
_GATEWAY_INSTANCES = [
    "fix_order_gateway_a",
    "fix_order_gateway_b",
    "binary_order_gateway_a",
    "binary_order_gateway_b",
]

_MATCHING_ENGINE_INSTANCES = [
    "matching_engine_primary",
    "matching_engine_secondary",
]


def _labels_for(component):
    return f'application="{APPLICATION}", component="{component}"'


def _gateway_spec(component):
    return {
        "labels": _labels_for(component),
        "histograms": [
            {
                "metric": "order_round_trip_nanoseconds_bucket",
                "scope": "gateway_thread",
                "name": "order round trip",
            },
        ],
        "counters": ["framework_pdu_messages_total"],
        "gauges": [],
    }


def _matching_engine_spec(component):
    return {
        "labels": _labels_for(component),
        # The matching engine has no histogram: the round trip is measured at the
        # gateway, which is where both ends of it are observable.
        "histograms": [],
        "counters": ["orders_processed_total", "framework_pdu_messages_total"],
        "gauges": [],
    }


COMPONENT_CONFIG = {component: _gateway_spec(component) for component in _GATEWAY_INSTANCES}
COMPONENT_CONFIG.update(
    {component: _matching_engine_spec(component) for component in _MATCHING_ENGINE_INSTANCES}
)

# The comparison view. One figure holding every gateway's round-trip histogram, which
# is the question the metric exists to answer: how does the ASCII FIX gateway compare
# with the binary one, on identical bucket bounds. An idle gateway contributes an
# empty histogram, which is honest -- it observed nothing.
COMPONENT_CONFIG["gateways"] = {
    "labels": f'application="{APPLICATION}"',
    "histograms": [
        {
            "metric": "order_round_trip_nanoseconds_bucket",
            "scope": "gateway_thread",
            "name": component,
            "labels": _labels_for(component),
        }
        for component in _GATEWAY_INSTANCES
    ],
    "counters": [],
    "gauges": [],
}



# --- Discovery: ask Prometheus what exists, rather than being told ---------- #
#
# The static table above is a fallback for working without a live Prometheus
# (--demo, or --input on a machine that has none). When Prometheus IS reachable
# the configuration is derived from it instead, which is strictly better: a
# component added to the venue appears here with no edit, and one renamed cannot
# leave a stale entry behind that silently returns nothing.
#
# It rests on the metric key design (docs/design/metrics.md): every series this
# venue exposes carries application, component and -- where a thread names one --
# scope. So one series query for {application="pubsub"} yields the whole shape:
# which components exist, which metrics each has, and which thread scope each
# metric belongs to.

def query_series(selector, prom_url):
    """Return the full label set of every series matching selector."""
    import requests  # lazy: only the live path needs it

    response = requests.get(f"{prom_url}/api/v1/series", params={"match[]": selector}, timeout=10.0)
    response.raise_for_status()
    return response.json()["data"]


def query_metric_types(prom_url):
    """Return {metric_name: type}, where type is 'counter', 'gauge', 'histogram', ...

    Taken from Prometheus's own metadata, which comes from the TYPE lines in the
    exposition, so it is what the exporting process declared rather than a guess
    made from the metric's name.
    """
    import requests  # lazy: only the live path needs it

    response = requests.get(f"{prom_url}/api/v1/metadata", timeout=10.0)
    response.raise_for_status()
    payload = response.json()["data"]
    types = {}
    for metric_name, entries in payload.items():
        if entries:
            types[metric_name] = entries[0].get("type", "unknown")
    return types


# Suffixes Prometheus appends to a histogram's own series. They must not be mistaken
# for counters in their own right: _sum and _count are parts of the histogram, and
# _bucket is the series the distribution is actually read from.
_HISTOGRAM_SUFFIXES = ("_bucket", "_sum", "_count")


def _base_metric_name(metric_name):
    for suffix in _HISTOGRAM_SUFFIXES:
        if metric_name.endswith(suffix):
            return metric_name[: -len(suffix)]
    return metric_name


def discover_component_config(prom_url, application=APPLICATION):
    """Build a COMPONENT_CONFIG by asking Prometheus what this application exposes.

    Returns {} if nothing is found, so the caller can fall back to the static table
    and say why.
    """
    series = query_series(f'{{application="{application}"}}', prom_url)
    if not series:
        return {}
    types = query_metric_types(prom_url)

    # component -> {"histograms": {(metric, scope): name}, "counters": set, "gauges": set}
    found = {}
    for labels in series:
        component = labels.get("component")
        metric_name = labels.get("__name__")
        if not component or not metric_name:
            continue
        entry = found.setdefault(component, {"histograms": {}, "counters": set(), "gauges": set()})

        base = _base_metric_name(metric_name)
        declared = types.get(base) or types.get(metric_name) or ""

        if declared == "histogram" or metric_name.endswith(_HISTOGRAM_SUFFIXES):
            # One entry per (metric, scope): a thread's histogram is its own series.
            entry["histograms"][(base + "_bucket", labels.get("scope", ""))] = base
        elif declared == "counter":
            entry["counters"].add(metric_name)
        elif declared == "gauge":
            entry["gauges"].add(metric_name)
        else:
            # Unknown type. Counted rather than dropped, because a metric nobody can
            # classify is still a metric somebody wanted; _total is the strong hint.
            (entry["counters"] if metric_name.endswith("_total") else entry["gauges"]).add(metric_name)

    config = {}
    for component, entry in sorted(found.items()):
        config[component] = {
            "labels": _labels_for(component),
            "histograms": [
                {"metric": metric, "scope": scope, "name": display_name}
                for (metric, scope), display_name in sorted(entry["histograms"].items())
            ],
            "counters": sorted(entry["counters"]),
            "gauges": sorted(entry["gauges"]),
        }

    _add_comparison_views(config)
    return config


def _add_comparison_views(config):
    """Add a pseudo-component per histogram shared by two or more components.

    A histogram exposed by several components is, by construction, one meant to be
    compared across them -- that is why it is one metric name distinguished by a
    label rather than one metric per component. order_round_trip_nanoseconds across
    the four order gateways is the case this venue exists to answer, and it needs no
    special-casing: any future shared histogram gets the same view for free.
    """
    owners = {}
    for component, spec in config.items():
        for histogram in spec["histograms"]:
            owners.setdefault((histogram["metric"], histogram["scope"]), []).append(component)

    for (metric, scope), components in sorted(owners.items()):
        if len(components) < 2:
            continue
        view_name = "compare:" + _base_metric_name(metric)
        config[view_name] = {
            "labels": f'application="{APPLICATION}"',
            "histograms": [
                {
                    "metric": metric,
                    "scope": scope,
                    "name": component,
                    "labels": _labels_for(component),
                }
                for component in sorted(components)
            ],
            "counters": [],
            "gauges": [],
        }


def resolve_component_config(prom_url, use_live):
    """Return (config, source) -- discovered from Prometheus, or the static fallback."""
    if not use_live:
        return COMPONENT_CONFIG, "static table"
    try:
        discovered = discover_component_config(prom_url)
    except Exception as error:  # pylint: disable=broad-except
        # Any failure to reach Prometheus falls back rather than aborting: the static
        # table still describes this venue, and saying so is more useful than a stack
        # trace about a connection.
        print(f"note: could not discover from {prom_url} ({error}); using the static table")
        return COMPONENT_CONFIG, "static table"
    if not discovered:
        print(f"note: {prom_url} returned no series for application={APPLICATION}; "
              "using the static table")
        return COMPONENT_CONFIG, "static table"
    return discovered, f"discovered from {prom_url}"


def print_discovered(config, source):
    """Print what is available to plot, and where the knowledge came from."""
    print(f"components ({source}):\n")
    for component in sorted(config):
        spec = config[component]
        print(f"  {component}")
        for histogram in spec["histograms"]:
            scope = histogram["scope"] or "(no scope)"
            print(f"      histogram  {histogram['name']}  [{scope}]")
        for metric in spec["counters"]:
            print(f"      counter    {metric}")
        for metric in spec["gauges"]:
            print(f"      gauge      {metric}")
        if not (spec["histograms"] or spec["counters"] or spec["gauges"]):
            print("      (nothing exposed)")
        print()

# --- Prometheus REST access (imports 'requests' lazily) -------------------- #

def query_prometheus(query, prom_url):
    """Execute a synchronous Prometheus instant query and return its result."""
    import requests  # lazy: only the fetch path needs it

    url = f"{prom_url}/api/v1/query"
    response = requests.get(url, params={"query": query}, timeout=5.0)
    response.raise_for_status()
    return response.json()["data"]["result"]


# How far back to look when the venue has stopped. A cumulative counter goes stale a
# few minutes after its process exits, so a plain instant query returns nothing even
# though Prometheus still holds every sample it scraped. Falling back to
# max_over_time recovers the last value it saw.
#
# max_over_time is right for cumulative series specifically because they only rise --
# the maximum over a window IS the final value. The exception is a process restart,
# which resets a counter to zero: across one, this returns the pre-restart peak rather
# than the current value. That is why it is a FALLBACK and not the default; a live
# venue answers the plain query and never reaches this path.
STALE_LOOKBACK_MINUTES = 60


def query_with_stale_fallback(instant_selector, stale_selector, prom_url):
    """Run an instant query; if nothing is live, run the stale-window form instead.

    The two expressions are passed separately rather than the second being derived by
    wrapping the first, because the obvious wrapping is wrong. `max_over_time((expr)[60m:])`
    is a SUBQUERY: it re-evaluates expr at fixed steps -- one minute by default -- and takes
    the maximum of those evaluations. A short run whose data spans less than one step can
    fall entirely between them and report zero. That is not hypothetical: a 20-second load
    run against the binary gateway read as "no observations" while its raw series held
    20,000, and it looked exactly like the gateway had recorded nothing.

    Applying max_over_time to the raw selector instead uses every stored sample, with no
    stepping, so the length of the run does not matter.
    """
    result = query_prometheus(instant_selector, prom_url)
    if result:
        return result
    return query_prometheus(stale_selector, prom_url)


def get_histogram_counts(metric, scope, sample, labels, prom_url, window):
    """Fetch cumulative bucket counts (grouped by 'le') for one histogram series.

    ``window`` selects what the counts mean:
      * "instant" -- the lifetime distribution: the raw cumulative bucket
                     counters as they stand now. Never spuriously empty as long
                     as any observation was ever recorded; ignores ``sample``.
      * "rate"    -- only the observations that landed in the last ``sample``
                     minutes (increase over the window). Reflects current
                     behaviour but is zero when the window is quiet.

    Either way the result is a set of cumulative per-'le' values, so the
    downstream cumulative_to_counts step is identical.
    """
    if window == "instant":
        series = f'{metric}{{{labels}, scope="{scope}"}}'
        # max_over_time INSIDE the aggregation, so it acts on raw samples per series.
        return query_with_stale_fallback(
            f"sum by (le) ({series})",
            f"sum by (le) (max_over_time({series}[{STALE_LOOKBACK_MINUTES}m]))",
            prom_url,
        )
    selector = f'sum by (le) (increase({metric}{{{labels}, scope="{scope}"}}[{sample}m]))'
    return query_prometheus(selector, prom_url)


def get_counter_total(metric, labels, prom_url):
    """Fetch the current absolute value of a counter."""
    result = query_with_stale_fallback(
        f'{metric}{{{labels}}}',
        f'max_over_time({metric}{{{labels}}}[{STALE_LOOKBACK_MINUTES}m])',
        prom_url,
    )
    return float(result[0]["value"][1]) if result else 0.0


def get_counter_rate(metric, sample, labels, prom_url):
    """Fetch a counter's rate (events/sec) over the sample window."""
    result = query_prometheus(f'rate({metric}{{{labels}}}[{sample}m])', prom_url)
    return float(result[0]["value"][1]) if result else 0.0


def get_gauge_value(metric, labels, prom_url):
    """Fetch the current instantaneous gauge value.

    Unlike a counter a gauge can fall, so a stale-window maximum would misreport it.
    It is queried live only: a gauge with no current value is genuinely unknown.
    """
    result = query_prometheus(f'{metric}{{{labels}}}', prom_url)
    return float(result[0]["value"][1]) if result else 0.0


def parse_buckets(results):
    """Convert a Prometheus 'sum by (le)' result into a sorted [(le, value)] list."""
    buckets = [(r["metric"]["le"], float(r["value"][1])) for r in results]
    buckets.sort(key=lambda pair: float("inf") if pair[0] == "+Inf" else float(pair[0]))
    return buckets


def cumulative_to_counts(buckets):
    """Turn cumulative histogram buckets into per-bucket counts."""
    values = []
    prev = 0.0
    for le, val in buckets:
        values.append((le, val - prev))
        prev = val
    return values


def buckets_to_bounds_counts(per_bucket):
    """Map per-bucket [(le, count)] to numeric (bounds, counts) for the renderer.

    The '+Inf' overflow bucket, if it holds anything, is placed at 1.5x the last
    finite bound so tail traffic is visible rather than silently dropped.
    """
    bounds, counts = [], []
    last_finite = None
    overflow = None
    for le, count in per_bucket:
        if le == "+Inf":
            overflow = count
            continue
        bound = float(le)
        bounds.append(bound)
        counts.append(count)
        last_finite = bound
    if overflow:
        bounds.append(last_finite * 1.5 if last_finite is not None else 1.0)
        counts.append(overflow)
    return bounds, counts


def fetch_from_prometheus(component, sample, prom_url, metrics_requested, hist_filter, window,
                          config=None):
    """Query Prometheus for one component and return a populated DashboardData."""
    spec = (config or COMPONENT_CONFIG)[component]
    labels = spec["labels"]

    data = DashboardData()
    set_component(data, component)

    if "histogram" in metrics_requested:
        for histogram in spec["histograms"]:
            name = histogram["name"]
            if hist_filter and name not in hist_filter:
                continue
            # A histogram may override the component's labels; the comparison view uses
            # that to put one series per gateway on a single figure.
            series_labels = histogram.get("labels", labels)
            per_bucket = cumulative_to_counts(
                parse_buckets(get_histogram_counts(histogram["metric"], histogram["scope"], sample,
                                                   series_labels, prom_url, window))
            )
            bounds, counts = buckets_to_bounds_counts(per_bucket)
            add_histogram(data, name, bounds, counts)

    if "counter" in metrics_requested:
        for metric in spec["counters"]:
            total = get_counter_total(metric, labels, prom_url)
            rate = get_counter_rate(metric, sample, labels, prom_url)
            add_counter(data, metric, total, rate)

    if "gauge" in metrics_requested:
        for metric in spec["gauges"]:
            add_gauge(data, metric, get_gauge_value(metric, labels, prom_url))

    return data


# --- Synthetic demo source (imports 'random' lazily) ----------------------- #

# The venue's real bucket bounds (environments/*.toml, metrics_order_round_trip_buckets), so
# demo shows the shape a real fetch would. It spans two regimes deliberately: microseconds
# for service time, and up to seconds for the queueing a saturating load test produces.
DEMO_BUCKET_BOUNDS_NS = [
    10_000, 25_000, 50_000, 100_000,
    250_000, 500_000, 1_000_000, 2_500_000,
    5_000_000, 10_000_000, 25_000_000,
    50_000_000, 100_000_000, 250_000_000,
    500_000_000, 1_000_000_000, 2_500_000_000,
    5_000_000_000,
]


def _demo_histogram(random_module, name, centre_ns, spread, total):
    counts = [0] * len(DEMO_BUCKET_BOUNDS_NS)
    for _ in range(total):
        latency = random_module.lognormvariate(0.0, spread) * centre_ns
        for index, bound in enumerate(DEMO_BUCKET_BOUNDS_NS):
            if latency <= bound:
                counts[index] += 1
                break
        else:
            counts[-1] += 1
    return name, list(DEMO_BUCKET_BOUNDS_NS), counts


def _demo_synthetic_counter(rng):
    """Return a plausible (total, rate/s) pair of artificial counter values."""
    total = rng.randint(1_000, 90_000_000)
    rate = round(rng.uniform(0.0, 25_000.0), 1)
    return total, rate


def _demo_synthetic_gauge(rng, name):
    """Return a plausible artificial gauge value, loosely shaped by its name."""
    lowered = name.lower()
    if "pct" in lowered:
        return round(rng.uniform(0.0, 100.0), 1)
    if any(token in lowered for token in ("queue", "depth", "flight", "backlog", "pending", "acks", "msgs")):
        return rng.randint(0, 50_000)
    return rng.randint(0, 5_000)


def _build_demo_for_component(component, metrics_requested, hist_filter, config=None):
    """Synthesise artificial data using ``component``'s configured series."""
    import random  # lazy: only the demo path needs it

    if metrics_requested is None:
        metrics_requested = {"histogram", "counter", "gauge"}
    spec = (config or COMPONENT_CONFIG)[component]
    # Seed from the component name so each component looks distinct but stable.
    rng = random.Random(42 + sum(ord(char) for char in component))

    data = DashboardData()
    set_component(data, component)

    # Centres inside the real bucket bounds: a demo clustered below the first bound would
    # render as one bar and teach nothing about reading the distribution.
    centres_ns = [80_000, 150_000, 400_000, 900_000, 20_000_000, 90_000_000]
    if "histogram" in metrics_requested:
        for histogram in spec["histograms"]:
            name = histogram["name"]
            if hist_filter and name not in hist_filter:
                continue
            _, bounds, counts = _demo_histogram(
                rng, name, rng.choice(centres_ns), rng.uniform(0.6, 1.0), rng.randint(12_000, 55_000)
            )
            add_histogram(data, name, bounds, counts)

    if "counter" in metrics_requested:
        for name in spec["counters"]:
            total, rate = _demo_synthetic_counter(rng)
            add_counter(data, name, total, rate)

    if "gauge" in metrics_requested:
        for name in spec["gauges"]:
            add_gauge(data, name, _demo_synthetic_gauge(rng, name))

    return data


def build_demo_dataset(component=None, metrics_requested=None, hist_filter=None, config=None):
    """Return a DashboardData of synthetic data -- for testing without Prometheus.

    With ``component`` given, the synthetic data uses that component's configured
    series (histogram paths, counter and gauge names) so the demo mirrors what a
    live fetch of that component would show -- but with artificial numbers.
    Without it, a generic 'sample' dataset is produced.
    """
    if component is not None:
        return _build_demo_for_component(component, metrics_requested, hist_filter, config)

    import random  # lazy: only the demo path needs it

    random.seed(42)
    data = DashboardData()

    for name, centre_ns, spread, total in [
        ("order_ingress_latency", 3000, 0.7, 50000),
        ("match_engine_latency", 6000, 0.8, 48000),
        ("market_data_publish_latency", 2000, 0.6, 52000),
        ("wal_fsync_latency", 15000, 0.9, 15000),
    ]:
        hname, bounds, counts = _demo_histogram(random, name, centre_ns, spread, total)
        add_histogram(data, hname, bounds, counts)

    for name, total, rate in [
        ("orders_accepted_total", 4_821_337, 1250.4),
        ("orders_rejected_total", 12_904, 3.2),
        ("executions_total", 9_113_882, 2380.7),
        ("market_data_msgs_total", 88_442_019, 21500.9),
        ("wal_records_written_total", 4_834_241, 1255.1),
        ("reconnects_total", 37, 0.0),
        ("heartbeats_sent_total", 1_204_557, 12.0),
    ]:
        add_counter(data, name, total, rate)

    for name, value in [
        ("open_orders", 18_442),
        ("subscribers_connected", 214),
        ("wal_backlog_bytes", 3_112_960),
        ("memory_slab_free_pct", 61.4),
        ("cpu_reactor_pct", 73.2),
        ("epoch", 7),
        ("leader", 1),
    ]:
        add_gauge(data, name, value)

    return data


# =========================================================================== #
# SERIALIZATION  (DashboardData <-> replayable snapshot file; pure stdlib)
# =========================================================================== #
#
# Each non-comment line is one builder call: the first whitespace-separated
# token is a function name, the rest are its arguments. Numeric lists are
# comma-joined; an empty (no-data) histogram uses "-" so the whitespace split
# always sees the same number of tokens.

def _number_to_text(value):
    """Serialise an int or float losslessly (int stays int, float stays float)."""
    return repr(value)


def _text_to_number(text):
    """Parse a token to int where possible, otherwise float."""
    try:
        return int(text)
    except ValueError:
        return float(text)


def _numbers_to_text(values):
    return ",".join(_number_to_text(value) for value in values) if values else "-"


def _text_to_numbers(text):
    return [] if text == "-" else [_text_to_number(token) for token in text.split(",")]


def write_dataset(data, path):
    """Write ``data`` to ``path`` as a sequence of replayable builder calls."""
    lines = ["# prometheus metrics snapshot -- replayable call transcript"]
    lines.append(" ".join(["set_component", data.component]))
    for histogram in data.histograms:
        lines.append(" ".join([
            "add_histogram",
            histogram["name"],
            _numbers_to_text(histogram["bounds"]),
            _numbers_to_text(histogram["counts"]),
        ]))
    for name, total, rate in data.counters:
        lines.append(" ".join(["add_counter", name, _number_to_text(total), _number_to_text(rate)]))
    for name, value in data.gauges:
        lines.append(" ".join(["add_gauge", name, _number_to_text(value)]))

    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def _replay_add_histogram(data, name, bounds, counts):
    add_histogram(data, name, _text_to_numbers(bounds), _text_to_numbers(counts))


def _replay_add_counter(data, name, total, rate):
    add_counter(data, name, _text_to_number(total), _text_to_number(rate))


def _replay_add_gauge(data, name, value):
    add_gauge(data, name, _text_to_number(value))


REPLAY_DISPATCH = {
    "set_component": set_component,
    "add_histogram": _replay_add_histogram,
    "add_counter": _replay_add_counter,
    "add_gauge": _replay_add_gauge,
}


def read_dataset(path):
    """Rebuild a DashboardData by replaying the calls recorded in ``path``."""
    data = DashboardData()
    with open(path, "r", encoding="utf-8") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            function_name, *arguments = line.split()
            handler = REPLAY_DISPATCH.get(function_name)
            if handler is None:
                raise ValueError(f"{path}:{line_number}: unknown function {function_name!r}")
            try:
                handler(data, *arguments)
            except TypeError as error:
                raise ValueError(f"{path}:{line_number}: bad arguments for {function_name!r}: {error}") from error
    return data


# =========================================================================== #
# RENDERING  (DashboardData -> on-screen figure; imports matplotlib/tkinter
#             lazily so the headless fetch+write side never needs them)
# =========================================================================== #

def format_latency_ns(value_ns):
    """Render a nanosecond value with the unit that suits its magnitude."""
    if value_ns < 1000:
        return f"{value_ns:g}ns"
    if value_ns < 1_000_000:
        return f"{value_ns / 1000:g}us"
    return f"{value_ns / 1_000_000:g}ms"


def format_value(value):
    """Format numbers with thousands separators; floats to one decimal."""
    if isinstance(value, float):
        return f"{value:,.1f}"
    return f"{value:,}"


def ascii_table(title, headers, rows):
    """Return a monospaced ASCII table. First column left-, rest right-aligned."""
    columns = len(headers)
    widths = [
        max([len(headers[column])] + [len(str(row[column])) for row in rows])
        for column in range(columns)
    ]

    def format_row(cells):
        parts = [
            str(cell).ljust(widths[column]) if column == 0 else str(cell).rjust(widths[column])
            for column, cell in enumerate(cells)
        ]
        return "| " + " | ".join(parts) + " |"

    horizontal = "+" + "+".join("-" * (width + 2) for width in widths) + "+"
    lines = [title, horizontal, format_row(headers), horizontal]
    for row in rows:
        lines.append(format_row(row))
    lines.append(horizontal)
    return "\n".join(lines)


def draw_histogram(axis, histogram):
    """Draw one latency histogram on ``axis`` with bars, count line, p99 line."""
    import matplotlib.ticker as mticker

    bounds = histogram["bounds"]
    counts = histogram["counts"]
    name = histogram["name"]

    axis.set_xscale("log")

    # An empty histogram is normal (no traffic in window), not a failure.
    #
    # It is drawn on the SAME x range as a populated one rather than left to matplotlib's
    # default 1..10. On a comparison figure the empty panels are the point of half the
    # exercise -- an idle gateway should read as "this gateway, nothing in it", against the
    # same scale as its neighbour, not as an unrelated chart with a nonsense axis.
    if not any(counts):
        axis.set_ylim(0, 1)
        if bounds:
            axis.set_xlim(bounds[0] / 2.0, bounds[-1] * 1.5)
            axis.set_xlabel("latency (log scale)")
            # Same tick formatting as a populated panel. Two panels side by side, one
            # reading "10us" and the other "10^4", are not being compared -- they are
            # being read as different charts.
            axis.xaxis.set_major_formatter(
                mticker.FuncFormatter(lambda value, _: format_latency_ns(value)))
            axis.xaxis.set_minor_formatter(mticker.NullFormatter())
        axis.set_title(f"{name} (no observations)", fontsize=10, fontweight="bold")
        axis.text(0.5, 0.5, "no observations recorded", transform=axis.transAxes,
                  ha="center", va="center", color="0.45", fontsize=9)
        return

    # Bar widths that look sensible on a log X axis: each bar spans from the
    # previous bound to its own upper bound.
    left_edges = [bounds[0] / 2.0] + bounds[:-1]
    widths = [bound - left for left, bound in zip(left_edges, bounds)]
    centres = [left + width / 2.0 for left, width in zip(left_edges, widths)]

    axis.bar(
        centres, counts, width=widths, align="center",
        color="#4C72B0", edgecolor="#2A2A2A", linewidth=0.5, alpha=0.85,
        label="bucket count",
    )

    axis.set_xlim(bounds[0] / 2.0, bounds[-1] * 1.2)
    axis.set_xlabel("latency (log scale)")
    axis.set_ylabel("bucket count", color="#4C72B0")
    axis.tick_params(axis="y", labelcolor="#4C72B0")
    axis.set_title(name, fontsize=10, fontweight="bold")
    axis.grid(True, which="both", axis="x", linestyle=":", alpha=0.3)

    # Tick labels are nanosecond bounds rendered in ns/us/ms as magnitude suits.
    axis.xaxis.set_major_formatter(mticker.FuncFormatter(lambda value, _: format_latency_ns(value)))
    axis.xaxis.set_minor_formatter(mticker.NullFormatter())

    # Superimposed cumulative-count line on a second Y axis.
    cumulative = []
    running = 0
    for count in counts:
        running += count
        cumulative.append(running)

    right_axis = axis.twinx()
    right_axis.plot(
        bounds, cumulative, color="#DD8452", marker="o", markersize=3,
        linewidth=1.5, label="cumulative count",
    )
    right_axis.set_ylabel("cumulative count", color="#DD8452")
    right_axis.tick_params(axis="y", labelcolor="#DD8452")

    # Legend combining both Y axes, otherwise the twin-axis labels never show.
    bar_handles, bar_labels = axis.get_legend_handles_labels()
    line_handles, line_labels = right_axis.get_legend_handles_labels()
    axis.legend(bar_handles + line_handles, bar_labels + line_labels, fontsize=8, loc="upper left")

    # Red dotted 99th-percentile line -- only when the percentile is known.
    p99 = percentile_bound(histogram, 0.99)
    if p99 is not None:
        axis.axvline(p99, color="red", linestyle=":", linewidth=1.8)
        axis.annotate(
            f"p99 ~ {format_latency_ns(p99)}",
            xy=(p99, max(counts)), xytext=(4, -2), textcoords="offset points",
            color="red", fontsize=8, rotation=90, va="top",
        )


def percentile_bound(histogram, fraction):
    """Return the bucket bound at cumulative ``fraction``, or None if unknown."""
    counts = histogram["counts"]
    total = sum(counts)
    if total == 0:
        return None
    target = total * fraction
    running = 0
    for bound, count in zip(histogram["bounds"], counts):
        running += count
        if running >= target:
            return bound
    return None


def summary_title(data):
    """Title describing what the figure actually shows.

    A component with no histogram is not a latency distribution, and saying it is invites
    the reader to look for one. The matching engine exposes counters only.
    """
    if data.histograms:
        return f"Latency Distribution: {data.component}"
    return f"Metrics: {data.component}"


def draw_overlay(axis, histograms):
    """Draw every histogram on one axes, as step outlines rather than bars.

    Filled bars cannot be overlaid -- whichever is drawn last hides the others -- so each
    series becomes an outline of its bucket counts. That is the shape comparison actually
    needs: where each distribution sits and how wide it is, on one x scale.

    Only the counts axis is drawn. The per-series cumulative line that the panel view
    superimposes on a second y axis would be four more curves on an axis whose scale
    means something different, which is how a comparison figure becomes unreadable.

    Series with no observations are named beneath the legend rather than plotted. Drawing a
    flat zero line for them would suggest a measured zero, which is a different claim from
    having measured nothing.
    """
    import matplotlib.ticker as mticker
    from matplotlib.lines import Line2D

    axis.set_xscale("log")

    populated = [h for h in histograms if any(h["counts"])]
    empty = [h["name"] for h in histograms if not any(h["counts"])]

    bounds_seen = []
    for histogram in populated:
        bounds = histogram["bounds"]
        counts = histogram["counts"]
        bounds_seen.extend(bounds)
        # 'pre', not 'post': a bucket count belongs to the interval ENDING at its bound,
        # so the value must be held from the previous bound up to this one. With 'post' the
        # whole distribution is drawn one bucket to the right of where it happened, which
        # looks entirely plausible and is wrong by a factor of the bucket ratio.
        line, = axis.step(bounds, counts, where="pre", linewidth=1.8, label=histogram["name"])
        percentile = percentile_bound(histogram, 0.99)
        if percentile is not None:
            axis.axvline(percentile, color=line.get_color(), linestyle=":", linewidth=1.2, alpha=0.8)

    if not bounds_seen:
        # Every series empty: keep the axis meaningful using whatever bounds were fetched.
        for histogram in histograms:
            bounds_seen.extend(histogram["bounds"])

    if bounds_seen:
        axis.set_xlim(min(bounds_seen) / 2.0, max(bounds_seen) * 1.5)

    axis.set_xlabel("latency (log scale)")
    axis.set_ylabel("bucket count")
    axis.grid(True, which="both", axis="x", linestyle=":", alpha=0.3)
    axis.xaxis.set_major_formatter(mticker.FuncFormatter(lambda value, _: format_latency_ns(value)))
    axis.xaxis.set_minor_formatter(mticker.NullFormatter())

    # Empty series go in the LEGEND, as greyed entries, rather than as a caption laid over
    # the plot -- a caption lands on whatever happens to be drawn there. Naming them at all
    # matters: "this gateway recorded nothing" is a result, and a legend that simply omitted
    # them would leave the reader to notice an absence.
    handles, labels = axis.get_legend_handles_labels()
    for name in empty:
        handles.append(Line2D([], [], color="0.6", linestyle=":", linewidth=1.2))
        labels.append(f"{name} (none)")
    if handles:
        axis.legend(handles, labels, fontsize=9, loc="upper left", framealpha=0.9)
    if populated:
        axis.set_title("dotted verticals mark each series' p99", fontsize=9, color="0.35")


def build_figure(data, overlay=False):
    """Build and return the matplotlib Figure for the whole dashboard.

    Layout: the counter and gauge tables sit across the top, then the histograms fill a
    two-column grid beneath them.

    The tables come first because they are the summary -- how many orders, at what rate --
    and a reader wants that before the distributions. They are also short, so putting them
    in a column beside the histograms left most of that column empty and pushed the
    histograms into a single narrow strip.

    Two columns rather than one because these histograms are meant to be compared: four
    gateways stacked vertically cannot be seen at once on any normal screen, which defeats
    the point of drawing them together.
    """
    import matplotlib.pyplot as plt

    histograms = data.histograms
    # Overlaying is one axes however many series there are.
    if overlay and histograms:
        columns, rows = 1, 1
    else:
        columns = 1 if len(histograms) <= 1 else 2
        rows = (len(histograms) + columns - 1) // columns

    # The table block is sized to its content rather than given a fixed share, so a
    # component with no counters or gauges does not reserve a band of blank paper.
    counter_rows = [(name, f"{total:,.0f}", f"{rate:,.1f}") for name, total, rate in data.counters]
    gauge_rows = [(name, format_value(value)) for name, value in data.gauges]
    table_text = (
        ascii_table("COUNTERS", ["NAME", "TOTAL", "RATE/s"], counter_rows)
        + "\n\n"
        + ascii_table("GAUGES", ["NAME", "VALUE"], gauge_rows)
    )
    # Measured from the text that will actually be drawn, not estimated from the row
    # counts: the tables carry headings, borders and a separator whose number is a
    # property of ascii_table, and guessing it too low ran the last line of the gauges
    # table into the first histogram's title.
    table_line_count = table_text.count("\n") + 1
    table_height_inches = max(1.2, TABLE_LINE_HEIGHT_INCHES * table_line_count + 0.3)
    # No histograms at all is a normal shape -- the matching engine exposes counters only --
    # and it must not reserve a band of blank paper below the tables.
    histogram_height_inches = (4.6 if overlay else 3.6) * rows

    figure = plt.figure(figsize=(7.5 * columns, table_height_inches + histogram_height_inches))
    if rows == 0:
        outer = figure.add_gridspec(1, 1)
    else:
        outer = figure.add_gridspec(
            2, 1,
            height_ratios=[table_height_inches, histogram_height_inches],
            hspace=0.35 / rows,
        )

    # --- summary tables, across the full width ---
    text_axis = figure.add_subplot(outer[0])
    text_axis.axis("off")
    text_axis.text(
        0.0, 1.0, table_text, family="monospace", fontsize=10,
        va="top", ha="left", transform=text_axis.transAxes,
    )

    # --- histograms, in a grid beneath ---
    if rows == 0:
        figure.suptitle(summary_title(data), fontsize=14, fontweight="bold")
        return figure

    inner = outer[1].subgridspec(rows, columns, wspace=0.32, hspace=0.55)
    if overlay:
        draw_overlay(figure.add_subplot(inner[0, 0]), histograms)
    else:
        for index, histogram in enumerate(histograms):
            axis = figure.add_subplot(inner[index // columns, index % columns])
            draw_histogram(axis, histogram)

    # A trailing odd panel would otherwise be drawn as an empty framed box that reads as a
    # histogram with nothing in it -- which is a meaningful state here, so it must not be
    # counterfeited by a layout artefact.
    for index in range(len(histograms), rows * columns):
        blank = figure.add_subplot(inner[index // columns, index % columns])
        blank.axis("off")

    figure.suptitle(summary_title(data), fontsize=14, fontweight="bold")
    return figure


def save_dashboard(data, path, overlay=False):
    """Render ``data`` to an image file, with no display required.

    Selecting the Agg backend before pyplot is imported is what makes this work on a
    headless box: the venue's host may well have no X display, and the figure is often
    wanted for a report rather than for looking at once.
    """
    import matplotlib
    matplotlib.use("Agg")
    figure = build_figure(data, overlay=overlay)
    figure.savefig(path, dpi=110, bbox_inches="tight", facecolor="white")
    print(f"wrote {path}")


def show_dashboard(data, overlay=False):
    """Render ``data`` in a scrollable tkinter window."""
    import signal
    import tkinter as tk
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

    figure = build_figure(data, overlay=overlay)

    root = tk.Tk()
    root.title(f"Latency Distribution: {data.component}")

    # The figure's full pixel extent -- the scroll region covers all of it so
    # nothing is ever clipped regardless of window size.
    width_px = int(figure.get_figwidth() * figure.dpi)
    height_px = int(figure.get_figheight() * figure.dpi)

    # Open wide enough to show the whole figure width (table included) but never
    # larger than the screen.
    scrollbar_allowance = 24
    window_width = min(width_px + scrollbar_allowance, root.winfo_screenwidth() - 80)
    window_height = min(height_px + scrollbar_allowance, root.winfo_screenheight() - 120)
    root.geometry(f"{window_width}x{window_height}")

    scroll_canvas = tk.Canvas(root, background="white")
    h_scroll = tk.Scrollbar(root, orient=tk.HORIZONTAL, command=scroll_canvas.xview)
    v_scroll = tk.Scrollbar(root, orient=tk.VERTICAL, command=scroll_canvas.yview)
    scroll_canvas.configure(xscrollcommand=h_scroll.set, yscrollcommand=v_scroll.set)

    h_scroll.pack(side=tk.BOTTOM, fill=tk.X)
    v_scroll.pack(side=tk.RIGHT, fill=tk.Y)
    scroll_canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

    figure_canvas = FigureCanvasTkAgg(figure, master=scroll_canvas)
    figure_canvas.draw()
    figure_widget = figure_canvas.get_tk_widget()
    figure_widget.configure(width=width_px, height=height_px)

    scroll_canvas.create_window((0, 0), window=figure_widget, anchor="nw")

    # A right-hand margin beyond the figure keeps the horizontal thumb partial,
    # so the scrollbar is visibly active and draggable on launch even when the
    # whole figure already fits. The margin is plain white canvas.
    horizontal_scroll_margin = int(width_px * 0.20)
    scroll_canvas.configure(scrollregion=(0, 0, width_px + horizontal_scroll_margin, height_px))

    def on_vertical_scroll(event):
        scroll_canvas.yview_scroll(int(-event.delta / 120) or (-1 if event.num == 4 else 1), "units")

    def on_horizontal_scroll(event):
        scroll_canvas.xview_scroll(int(-event.delta / 120) or (-1 if event.num == 4 else 1), "units")

    scroll_canvas.bind_all("<MouseWheel>", on_vertical_scroll)
    scroll_canvas.bind_all("<Shift-MouseWheel>", on_horizontal_scroll)
    scroll_canvas.bind_all("<Button-4>", on_vertical_scroll)
    scroll_canvas.bind_all("<Button-5>", on_vertical_scroll)

    # Ctrl-C handling: a SIGINT handler tears the window down, and a periodic
    # timer keeps the interpreter regaining control so the signal is delivered.
    def shutdown(*_):
        root.quit()

    signal.signal(signal.SIGINT, shutdown)
    root.protocol("WM_DELETE_WINDOW", shutdown)

    def keep_alive():
        root.after(200, keep_alive)

    root.after(200, keep_alive)

    try:
        root.mainloop()
    except KeyboardInterrupt:
        shutdown()
    finally:
        root.destroy()
        plt.close(figure)


# =========================================================================== #
# Command-line interface (wires the three layers together)
# =========================================================================== #

def parse_arguments(argv=None):
    """Parse and validate the command line, returning the argparse namespace."""
    parser = argparse.ArgumentParser(
        description="Visualise this venue's Prometheus metrics, especially the latency histograms.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Compare every order gateway:\n"
            "  pubsub_metrics.py --component gateways --graphic\n"
            "Fetch + save on the venue host:\n"
            "  pubsub_metrics.py --component gateways --output gateways.dash\n"
            "View a saved snapshot elsewhere:\n"
            "  pubsub_metrics.py --input gateways.dash --graphic\n"
        ),
    )
    parser.add_argument("--graphic", action="store_true",
                        help="render the dashboard in a window")
    parser.add_argument("--overlay", action="store_true",
                        help="draw every histogram on ONE axes as step outlines instead of "
                             "separate panels, for comparing distributions directly. Off by "
                             "default: separate panels read better when the series are not "
                             "meant to be compared")
    parser.add_argument("--save-figure", metavar="FILENAME",
                        help="render the dashboard to an image file (PNG/SVG/PDF) instead of "
                             "a window; needs no display, so it works over ssh")
    parser.add_argument("--input", metavar="FILENAME",
                        help="read a saved snapshot and render it (requires --graphic)")
    parser.add_argument("--output", metavar="FILENAME",
                        help="write the acquired data to a replayable snapshot file")
    parser.add_argument("--component",
                        help="component to fetch, or a compare:<metric> view "
                             "(required when fetching live; --list shows what is available). "
                             "Deliberately not a fixed choice list: the valid set is whatever "
                             "Prometheus currently has, not what this script was written knowing")
    parser.add_argument("--list", action="store_true",
                        help="list the components and metrics available, then exit")
    parser.add_argument("--application", default=APPLICATION,
                        help=f"application label to discover (default: {APPLICATION})")
    parser.add_argument("--sample", type=int, default=5,
                        help="lookback window in minutes for --window rate (default: 5)")
    parser.add_argument("--window", choices=["instant", "rate"], default="instant",
                        help="histogram source: 'instant' = lifetime cumulative distribution "
                             "(default; never spuriously empty), 'rate' = only the last --sample "
                             "minutes of traffic (needs live traffic)")
    parser.add_argument("--prom-url", default=PROM_URL,
                        help=f"Prometheus base URL (default: {PROM_URL})")
    parser.add_argument("--metrics", default="all",
                        help="comma list of histogram,counter,gauge,all (default: all)")
    parser.add_argument("--hist", action="append", metavar="PATH",
                        help="restrict histograms to this path (repeatable)")
    parser.add_argument("--demo", action="store_true",
                        help="use synthetic data instead of Prometheus; with --component, "
                             "synthesises that component's configured series (for testing)")

    arguments = parser.parse_args(argv)

    if arguments.input and arguments.output:
        parser.error("--input and --output cannot be used together")
    if arguments.input and arguments.demo:
        parser.error("--input and --demo cannot be used together")
    if arguments.input and not (arguments.graphic or arguments.save_figure):
        parser.error("--input requires --graphic or --save-figure")
    if arguments.list:
        return arguments
    if not arguments.graphic and not arguments.output and not arguments.save_figure:
        parser.error("at least one of --graphic, --save-figure or --output is required")
    fetching_live = not arguments.input and not arguments.demo
    if fetching_live and not arguments.component:
        parser.error("--component is required when fetching from Prometheus")

    return arguments


def resolve_metrics(text):
    """Turn a --metrics string into a validated set of series kinds."""
    requested = {token.strip().lower() for token in text.split(",")}
    valid = {"histogram", "counter", "gauge", "all"}
    invalid = requested - valid
    if invalid:
        raise ValueError(f"invalid --metrics value(s): {sorted(invalid)}")
    if "all" in requested:
        return {"histogram", "counter", "gauge"}
    return requested


def main(argv=None):
    arguments = parse_arguments(argv)
    metrics_requested = resolve_metrics(arguments.metrics)

    global APPLICATION  # pylint: disable=global-statement
    APPLICATION = arguments.application

    # What is available to plot. Prometheus is asked whenever it will be consulted at
    # all -- for --list and for a live fetch -- so the component names come from the
    # venue rather than from this file. Replaying a snapshot needs no such lookup.
    consult_prometheus = arguments.list or not (arguments.input or arguments.demo)
    config, source = resolve_component_config(arguments.prom_url, consult_prometheus)

    if arguments.list:
        print_discovered(config, source)
        return 0

    if arguments.component is not None and arguments.component not in config and not arguments.input:
        available = ", ".join(sorted(config)) or "(none)"
        print(f"error: unknown component '{arguments.component}'.\n"
              f"Available ({source}): {available}", file=sys.stderr)
        return 2

    # ACQUISITION: one of three sources.
    if arguments.input:
        data = read_dataset(arguments.input)
    elif arguments.demo:
        data = build_demo_dataset(arguments.component, metrics_requested, arguments.hist, config)
    else:
        data = fetch_from_prometheus(
            arguments.component, arguments.sample, arguments.prom_url,
            metrics_requested, arguments.hist, arguments.window, config,
        )

    # An explicit --component overrides the label carried by file/demo data.
    if arguments.component is not None:
        data.component = arguments.component

    # SERIALIZATION: optionally persist it.
    if arguments.output:
        write_dataset(data, arguments.output)
        print(f"wrote {arguments.output}")

    # RENDERING: to a file, to a window, or both.
    if arguments.save_figure:
        save_dashboard(data, arguments.save_figure, overlay=arguments.overlay)
    if arguments.graphic:
        show_dashboard(data, overlay=arguments.overlay)


if __name__ == "__main__":
    sys.exit(main())
