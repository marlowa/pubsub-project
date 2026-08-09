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

  4. Track the round trip through the session against a ceiling it must not exceed,
     with the orders that did exceed it counted underneath:

         python3 pubsub_metrics.py --component fix_order_gateway_a \
                 --metrics bands --ceiling 2.5ms --since 480 --graphic

     The band chart needs no new instrumentation -- it is the existing histogram read
     back with a RANGE query instead of an instant one, so Prometheus has been storing
     it all along. Two things it cannot give you, whatever the resolution: a true
     minimum or maximum (the outer buckets are unbounded), and individual outliers as
     points (a histogram keeps counts, not observations).

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
    """Container for the four kinds of series shown on the dashboard."""

    def __init__(self):
        self.component = "sample"  # label shown in the figure title
        self.histograms = []       # list of {"name", "bounds", "counts"}
        self.bands = []            # list of {"name", "times", "tracks", "ceiling_ns",
                               #          "breaches", "observations"}
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


def add_latency_bands(data, name, times, tracks, ceiling_ns=None, breaches=None,
                      observations=None):
    """Append one percentile-over-time chart: how the distribution moved during the window.

    ``times`` are the epoch second each interval begins at, and ``tracks`` is an ordered
    [(label, [value per time])] list running from the lowest percentile to the highest, so
    the renderer can shade between the outermost two without being told which they are.
    A value may be None where the interval had no traffic to take a percentile of.

    ``ceiling_ns`` is the latency a round trip is not supposed to exceed, and ``breaches``
    the count of orders that exceeded it in each interval. The two are separate on purpose:
    a percentile line sitting under the ceiling does NOT mean nothing breached it -- p99
    under the ceiling still permits one order in a hundred above it, which for a day's
    order flow is a large number. The line answers "how is the venue behaving", the count
    answers "how many orders were late", and only the second is the compliance question.

    ``observations`` is how many orders were measured in each interval, and it is here
    because a fall in it can raise an averaged alarm all by itself, with no latency change
    whatever. Without it a reader seeing steady percentiles and a firing alarm has no way
    to reach the explanation, and will go looking for a slowdown that never happened.
    """
    data.bands.append(
        {
            "name": name,
            "times": list(times),
            "tracks": [(label, list(values)) for label, values in tracks],
            "ceiling_ns": ceiling_ns,
            "breaches": list(breaches) if breaches is not None else None,
            "observations": list(observations) if observations is not None else None,
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


def query_prometheus_range(query, start_epoch_seconds, end_epoch_seconds, step_seconds, prom_url):
    """Execute a Prometheus RANGE query, returning one value per step rather than one value.

    This is the whole difference between the histogram view and the band view. A histogram
    is an instant query: one distribution, however long it took to accumulate. A band chart
    needs the same expression evaluated repeatedly across the window, which is what
    /api/v1/query_range does -- and it is why no new instrumentation is required to draw
    one. Prometheus has been storing this all along.
    """
    import requests  # lazy: only the fetch path needs it

    url = f"{prom_url}/api/v1/query_range"
    parameters = {
        "query": query,
        "start": start_epoch_seconds,
        "end": end_epoch_seconds,
        "step": step_seconds,
    }
    # Longer than the instant-query timeout: a range query is many evaluations, and a
    # day-long window at a fine step is a lot of them.
    response = requests.get(url, params=parameters, timeout=30.0)
    response.raise_for_status()
    return response.json()["data"]["result"]


def parse_range_values(results):
    """Turn a single-series range-query result into [(epoch_second, value)].

    NaN is mapped to None. Prometheus returns it for an interval in which nothing was
    observed -- histogram_quantile of an empty distribution is undefined, not zero -- and
    plotting it as zero would draw a latency cliff to the floor where there was simply no
    traffic. The two are opposite findings and must not render alike.
    """
    import math

    if not results:
        return []
    pairs = []
    for timestamp, text in results[0]["values"]:
        value = float(text)
        pairs.append((float(timestamp), None if math.isnan(value) else value))
    return pairs


def align_to_step_grid(pairs, start_epoch_seconds, end_epoch_seconds, step_seconds):
    """Place fetched points on every step of the window, with None where there is no data.

    Prometheus omits empty regions from a range query rather than returning them as null,
    so a window in which the venue was not running comes back as points either side of the
    gap and nothing between. Plotted directly, two points three hours apart are adjacent
    samples, and the line drawn between them reads as steady latency throughout -- the most
    reassuring part of the chart, describing the interval with no data at all.

    Rebuilding the series on the full grid turns that into an explicit absence, which the
    renderer already knows how to break a line on. Doing it here rather than by guessing at
    suspicious gaps means the rule is exact: a step with no sample is a hole, whatever its
    neighbours look like.
    """
    by_timestamp = {int(timestamp): value for timestamp, value in pairs}
    grid = []
    moment = start_epoch_seconds
    while moment <= end_epoch_seconds:
        grid.append((moment, by_timestamp.get(moment)))
        moment += step_seconds
    return grid


def get_percentile_range(metric, scope, labels, quantile, start_epoch_seconds,
                         end_epoch_seconds, step_seconds, prom_url):
    """Fetch one percentile of a histogram, evaluated at every step across the window.

    histogram_quantile does the work server-side, and the rate() inside it is what makes
    each point describe only its own interval rather than the venue's whole lifetime.

    The lookback given to rate() is twice the step, not the step itself. rate() needs at
    least two samples in its window to return anything, so at a step equal to the scrape
    interval a one-step lookback silently yields gaps; doubling it guarantees overlap at
    the cost of neighbouring points sharing a little data.

    A caveat worth knowing when reading the result: histogram_quantile INTERPOLATES within
    the bucket the percentile falls in. It reports a value the venue may never have
    produced, and its accuracy is entirely a property of how well the bucket bounds suit
    the traffic -- which is why the bounds matter more than the query does.
    """
    selector = f'{metric}{{{labels}, scope="{scope}"}}'
    query = (f'histogram_quantile({quantile}, '
             f'sum by (le) (rate({selector}[{step_seconds * 2}s])))')
    return parse_range_values(
        query_prometheus_range(query, start_epoch_seconds, end_epoch_seconds,
                               step_seconds, prom_url))


def find_bucket_bound_label(metric, scope, labels, ceiling_ns, prom_url):
    """Return the exact `le` label string naming ``ceiling_ns``, or None if none does.

    The label is DISCOVERED rather than formatted from the number, because `le` is a
    string and the match is exact: le="100000" and le="100000.0" name the same latency and
    are different labels. Which of them the exporter writes is a property of the exporter's
    serialiser, not of the value, and getting it wrong returns an empty result that looks
    exactly like a venue with no late orders. Asking Prometheus what labels it actually
    holds cannot be wrong in that way.
    """
    series = query_series(f'{metric}{{{labels}, scope="{scope}"}}', prom_url)
    bounds = {entry["le"] for entry in series if "le" in entry}
    for bound in bounds:
        if bound != "+Inf" and float(bound) == float(ceiling_ns):
            return bound
    return None


def describe_nearest_bounds(metric, scope, labels, ceiling_ns, prom_url):
    """Return the two configured bucket bounds either side of ``ceiling_ns``, as text.

    Used only to explain a ceiling that no bucket bound sits on. Naming the neighbours
    turns "cannot count breaches" into "move the ceiling here, or add a bound there".
    """
    series = query_series(f'{metric}{{{labels}, scope="{scope}"}}', prom_url)
    bounds = sorted(float(entry["le"]) for entry in series
                    if entry.get("le") not in (None, "+Inf"))
    below = [bound for bound in bounds if bound < float(ceiling_ns)]
    above = [bound for bound in bounds if bound > float(ceiling_ns)]
    parts = []
    if below:
        parts.append(f"nearest below {format_latency_ns(below[-1])}")
    if above:
        parts.append(f"nearest above {format_latency_ns(above[0])}")
    return ", ".join(parts) if parts else "no finite bucket bounds configured"


def get_observation_range(metric, scope, labels, start_epoch_seconds,
                          end_epoch_seconds, step_seconds, prom_url):
    """Fetch how many observations the histogram recorded in each interval.

    Read from the _count series, which is the same population the percentiles are taken
    from -- so a fall here explains a percentile that went quiet, and rules in or out the
    one cause of an averaged alarm that involves no latency change at all.
    """
    total = f'{metric.replace("_bucket", "_count")}{{{labels}, scope="{scope}"}}'
    query = f'sum(increase({total}[{step_seconds * 2}s]))'
    return parse_range_values(
        query_prometheus_range(query, start_epoch_seconds, end_epoch_seconds,
                               step_seconds, prom_url))


def get_breach_range(metric, scope, labels, bound_label, start_epoch_seconds,
                     end_epoch_seconds, step_seconds, prom_url):
    """Fetch how many observations exceeded the ceiling in each interval.

    This is a subtraction of two cumulative bucket counters, not an estimate: everything
    observed, minus everything at or below the ceiling. No interpolation and no percentile
    is involved, so unlike the percentile tracks the answer is EXACT -- which is what makes
    it the right instrument for a question about how many orders were late.
    """
    total = f'{metric.replace("_bucket", "_count")}{{{labels}, scope="{scope}"}}'
    under = f'{metric}{{{labels}, scope="{scope}", le="{bound_label}"}}'
    query = (f'sum(increase({total}[{step_seconds * 2}s])) - '
             f'sum(increase({under}[{step_seconds * 2}s]))')
    return parse_range_values(
        query_prometheus_range(query, start_epoch_seconds, end_epoch_seconds,
                               step_seconds, prom_url))


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


# The percentiles the band chart tracks, lowest first so the renderer can shade between
# the outermost two without being told which they are.
#
# There is no min and no max. A Prometheus histogram cannot supply either: its top bucket
# is unbounded, so the largest thing it can ever say is "something exceeded the last
# bound", never by how much, and the bottom bucket is the same in reverse. A "max" line
# drawn from bucket data would be the last bound -- a number that says more about the
# configuration than about the traffic. Orders above the ceiling are counted exactly
# instead, which is the question a max was being asked to answer.
BAND_QUANTILES = [(0.50, "p50"), (0.90, "p90"), (0.99, "p99")]


def fetch_band_series(spec, labels, prom_url, band_options, hist_filter, data):
    """Fetch the percentile-over-time tracks for every histogram a component exposes."""
    import time  # lazy: only the fetch path needs it

    step_seconds = band_options["step_seconds"]
    end_epoch_seconds = int(time.time())
    start_epoch_seconds = end_epoch_seconds - band_options["since_minutes"] * 60
    ceiling_ns = band_options["ceiling_ns"]

    for histogram in spec["histograms"]:
        name = histogram["name"]
        if hist_filter and name not in hist_filter:
            continue
        series_labels = histogram.get("labels", labels)
        metric, scope = histogram["metric"], histogram["scope"]

        # Every series is placed on the same grid rather than each carrying its own
        # timestamps, so the parallel lists the renderer indexes really are parallel. They
        # are fetched by separate queries and a point missing from one but present in
        # another would otherwise shift a track against its own breach counts.
        tracks, times = [], []
        for quantile, label in BAND_QUANTILES:
            pairs = align_to_step_grid(
                get_percentile_range(metric, scope, series_labels, quantile,
                                     start_epoch_seconds, end_epoch_seconds,
                                     step_seconds, prom_url),
                start_epoch_seconds, end_epoch_seconds, step_seconds)
            if not times:
                times = [timestamp for timestamp, _ in pairs]
            tracks.append((label, [value for _, value in pairs]))

        observations = [value for _, value in align_to_step_grid(
            get_observation_range(metric, scope, series_labels,
                                  start_epoch_seconds, end_epoch_seconds,
                                  step_seconds, prom_url),
            start_epoch_seconds, end_epoch_seconds, step_seconds)]

        breaches = None
        if ceiling_ns is not None and times:
            bound_label = find_bucket_bound_label(metric, scope, series_labels,
                                                  ceiling_ns, prom_url)
            if bound_label is None:
                # Refused rather than approximated. The breach count is worth having only
                # because it is exact, and an interpolated one would be a percentile
                # wearing a counter's name.
                print(f"warning: no bucket bound at {format_latency_ns(ceiling_ns)} for "
                      f"'{name}', so orders over the ceiling cannot be counted "
                      f"({describe_nearest_bounds(metric, scope, series_labels, ceiling_ns, prom_url)}). "
                      f"The ceiling line is still drawn.", file=sys.stderr)
            else:
                breaches = [value for _, value in align_to_step_grid(
                    get_breach_range(metric, scope, series_labels, bound_label,
                                     start_epoch_seconds, end_epoch_seconds,
                                     step_seconds, prom_url),
                    start_epoch_seconds, end_epoch_seconds, step_seconds)]

        add_latency_bands(data, name, times, tracks, ceiling_ns, breaches, observations)


def fetch_from_prometheus(component, sample, prom_url, metrics_requested, hist_filter, window,
                          config=None, band_options=None):
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

    if "bands" in metrics_requested and band_options is not None:
        fetch_band_series(spec, labels, prom_url, band_options, hist_filter, data)

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


def _demo_bucket_counts(random_module, bounds, centre_ns, spread, total, counts=None):
    """Draw ``total`` lognormal latencies and bin them into ``bounds``.

    Observations above the last bound land in the last band, matching the histogram
    demo's long-standing behaviour. ``counts`` accumulates into an existing list, which
    is how one interval can be built from two populations -- a normal one, and a few
    deliberately late orders.
    """
    if counts is None:
        counts = [0] * len(bounds)
    for _ in range(total):
        latency = random_module.lognormvariate(0.0, spread) * centre_ns
        for index, bound in enumerate(bounds):
            if latency <= bound:
                counts[index] += 1
                break
        else:
            counts[-1] += 1
    return counts


def _demo_histogram(random_module, name, centre_ns, spread, total):
    counts = _demo_bucket_counts(random_module, DEMO_BUCKET_BOUNDS_NS, centre_ns, spread, total)
    return name, list(DEMO_BUCKET_BOUNDS_NS), counts


# --- The demo band chart's trading day -------------------------------------- #
#
# A session carrying the patterns a percentile chart exists to tell apart, all of which
# raise a short-window arithmetic mean by a similar amount:
#
#   open volatility   every track lifts together        -- the venue really is slower
#   tail excursion    p50 and p90 flat, p99 climbs      -- a few orders are late
#   volume collapse   nothing moves at all              -- an averaged alarm is lying
#
# The ceiling is placed ON a configured bucket bound, because that is the only way the
# breach count underneath can be exact rather than interpolated.

DEMO_BAND_STEP_SECONDS = 30
DEMO_BAND_SESSION_MINUTES = 480       # 08:00 to 16:00
DEMO_BAND_OPEN_HOUR = 8
DEMO_BAND_CEILING_NS = 2_500_000      # 2.5ms -- a bound present in DEMO_BUCKET_BOUNDS_NS
DEMO_BAND_BASELINE_CENTRE_NS = 300_000
DEMO_BAND_BASELINE_SPREAD = 0.55
DEMO_BAND_BASELINE_VOLUME = 900

# (first minute, last minute, centre_ns, spread, volume, late orders per interval)
DEMO_BAND_PHASES = [
    (0, 22, 900_000, 0.75, 2600, 0),          # the open: heavier flow, genuinely slower
    (168, 180, DEMO_BAND_BASELINE_CENTRE_NS, DEMO_BAND_BASELINE_SPREAD,
     DEMO_BAND_BASELINE_VOLUME, 30),          # tail excursion, mass unmoved
    (300, 330, DEMO_BAND_BASELINE_CENTRE_NS, DEMO_BAND_BASELINE_SPREAD, 90, 0),
    (462, 480, 700_000, 0.7, 2100, 8),        # the close
]


def _interpolated_percentile(bounds, counts, fraction):
    """Estimate a percentile the way Prometheus's histogram_quantile does.

    Linear interpolation WITHIN the bucket the rank falls in, rather than snapping to that
    bucket's upper bound. This exists so the demo behaves like the live path: snapping
    makes a percentile sitting near a bound flick between it and its neighbour from one
    interval to the next -- across a 1ms/2.5ms bucket step, a 2.5x sawtooth that is purely
    an artefact of the bounds and reads as violent instability the venue never had.

    The interpolation assumes observations are spread evenly across the bucket, which they
    are not. It is an estimate, and its quality is a property of how well the bucket bounds
    suit the traffic.
    """
    total = sum(counts)
    if total == 0:
        return None
    target = total * fraction
    running = 0.0
    lower = 0.0
    for bound, count in zip(bounds, counts):
        if running + count >= target and count > 0:
            return lower + (bound - lower) * (target - running) / count
        running += count
        lower = bound
    return bounds[-1] if bounds else None


def _demo_band_phase(elapsed_minutes):
    """Return the (centre_ns, spread, volume, late) in force ``elapsed_minutes`` in."""
    for first, last, centre_ns, spread, volume, late in DEMO_BAND_PHASES:
        if first <= elapsed_minutes <= last:
            return centre_ns, spread, volume, late
    return (DEMO_BAND_BASELINE_CENTRE_NS, DEMO_BAND_BASELINE_SPREAD,
            DEMO_BAND_BASELINE_VOLUME, 0)


def _demo_band_start_epoch_seconds():
    """Epoch second of the most recent 08:00 local, so the axis reads as a trading day."""
    import time  # lazy: only the demo path needs it

    now = time.localtime()
    open_struct = (now.tm_year, now.tm_mon, now.tm_mday,
                   DEMO_BAND_OPEN_HOUR, 0, 0, 0, 0, -1)
    open_epoch = int(time.mktime(open_struct))
    return open_epoch if open_epoch <= time.time() else open_epoch - 86400


def _demo_latency_bands(random_module, name, ceiling_ns=None):
    """Synthesise percentile tracks and a breach count across a trading session.

    Percentiles are taken from a freshly drawn distribution per interval, at the bucket
    granularity a real histogram would impose, so the tracks step rather than glide -- the
    resolution really is that coarse and the demo should not pretend otherwise.

    The breach count is derived from the same draws by summing the bands ABOVE the
    ceiling, which is precisely what the live query does by subtracting two bucket
    counters. It is deliberately not derived from the percentiles, because the point of
    showing both is that neither can be recovered from the other -- and it is deliberately
    refused when no bucket bound sits on the ceiling, even though the demo holds the raw
    draws and could count them anyway. A demo that quietly did what the live path cannot
    would teach the one lesson this chart exists to teach, backwards.
    """
    if ceiling_ns is None:
        ceiling_ns = DEMO_BAND_CEILING_NS
    bounds = list(DEMO_BUCKET_BOUNDS_NS)
    countable = any(bound == ceiling_ns for bound in bounds)
    if not countable:
        print(f"warning: no bucket bound at {format_latency_ns(ceiling_ns)}, so orders over "
              f"the ceiling cannot be counted exactly. The ceiling line is still drawn.",
              file=sys.stderr)

    intervals = DEMO_BAND_SESSION_MINUTES * 60 // DEMO_BAND_STEP_SECONDS
    start = _demo_band_start_epoch_seconds()

    times, breaches, observations = [], [], []
    values = {label: [] for _, label in BAND_QUANTILES}
    for interval in range(intervals):
        elapsed_minutes = interval * DEMO_BAND_STEP_SECONDS // 60
        centre_ns, spread, volume, late = _demo_band_phase(elapsed_minutes)
        counts = _demo_bucket_counts(random_module, bounds, centre_ns, spread, volume)
        if late:
            _demo_bucket_counts(random_module, bounds, ceiling_ns * 4, 0.5, late, counts)

        for quantile, label in BAND_QUANTILES:
            values[label].append(_interpolated_percentile(bounds, counts, quantile))
        breaches.append(sum(count for bound, count in zip(bounds, counts)
                            if bound > ceiling_ns))
        observations.append(sum(counts))
        times.append(start + interval * DEMO_BAND_STEP_SECONDS)

    tracks = [(label, values[label]) for _, label in BAND_QUANTILES]
    return (name, times, tracks, ceiling_ns,
            (breaches if countable else None), observations)


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


def _build_demo_for_component(component, metrics_requested, hist_filter, config=None,
                              ceiling_ns=None):
    """Synthesise artificial data using ``component``'s configured series."""
    import random  # lazy: only the demo path needs it

    if metrics_requested is None:
        metrics_requested = {"histogram", "bands", "counter", "gauge"}
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

    # The bands are drawn from the same underlying series as the histogram beside them --
    # the histogram is that series summed over the window, the bands are its percentiles
    # kept per interval -- so one is synthesised wherever a histogram is configured.
    if "bands" in metrics_requested:
        for histogram in spec["histograms"]:
            name = histogram["name"]
            if hist_filter and name not in hist_filter:
                continue
            _, times, tracks, ceiling, breaches, seen = _demo_latency_bands(rng, name, ceiling_ns)
            add_latency_bands(data, name, times, tracks, ceiling, breaches, seen)

    if "counter" in metrics_requested:
        for name in spec["counters"]:
            total, rate = _demo_synthetic_counter(rng)
            add_counter(data, name, total, rate)

    if "gauge" in metrics_requested:
        for name in spec["gauges"]:
            add_gauge(data, name, _demo_synthetic_gauge(rng, name))

    return data


def build_demo_dataset(component=None, metrics_requested=None, hist_filter=None, config=None,
                       ceiling_ns=None):
    """Return a DashboardData of synthetic data -- for testing without Prometheus.

    With ``component`` given, the synthetic data uses that component's configured
    series (histogram paths, counter and gauge names) so the demo mirrors what a
    live fetch of that component would show -- but with artificial numbers.
    Without it, a generic 'sample' dataset is produced.

    ``metrics_requested`` is honoured on both paths. It used to be ignored here, which
    only became visible once there was a series worth asking for on its own: --metrics
    bands drew the band chart and then four histograms and both tables underneath it.
    """
    if component is not None:
        return _build_demo_for_component(component, metrics_requested, hist_filter, config,
                                         ceiling_ns)

    import random  # lazy: only the demo path needs it

    if metrics_requested is None:
        metrics_requested = {"histogram", "bands", "counter", "gauge"}

    random.seed(42)
    data = DashboardData()

    if "histogram" in metrics_requested:
        for name, centre_ns, spread, total in [
            ("order_ingress_latency", 3000, 0.7, 50000),
            ("match_engine_latency", 6000, 0.8, 48000),
            ("market_data_publish_latency", 2000, 0.6, 52000),
            ("wal_fsync_latency", 15000, 0.9, 15000),
        ]:
            if hist_filter and name not in hist_filter:
                continue
            hname, bounds, counts = _demo_histogram(random, name, centre_ns, spread, total)
            add_histogram(data, hname, bounds, counts)

    if "bands" in metrics_requested:
        bname, times, tracks, ceiling, breaches, seen = _demo_latency_bands(
            random, "order_round_trip_latency", ceiling_ns)
        add_latency_bands(data, bname, times, tracks, ceiling, breaches, seen)

    counters = [] if "counter" not in metrics_requested else [
        ("orders_accepted_total", 4_821_337, 1250.4),
        ("orders_rejected_total", 12_904, 3.2),
        ("executions_total", 9_113_882, 2380.7),
        ("market_data_msgs_total", 88_442_019, 21500.9),
        ("wal_records_written_total", 4_834_241, 1255.1),
        ("reconnects_total", 37, 0.0),
        ("heartbeats_sent_total", 1_204_557, 12.0),
    ]
    for name, total, rate in counters:
        add_counter(data, name, total, rate)

    gauges = [] if "gauge" not in metrics_requested else [
        ("open_orders", 18_442),
        ("subscribers_connected", 214),
        ("wal_backlog_bytes", 3_112_960),
        ("memory_slab_free_pct", 61.4),
        ("cpu_reactor_pct", 73.2),
        ("epoch", 7),
        ("leader", 1),
    ]
    for name, value in gauges:
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


# A percentile track carries None where an interval had no traffic to take a percentile
# of, which is a different fact from a zero and has to survive the round trip.
_ABSENT = "~"


def _optional_numbers_to_text(values):
    if not values:
        return "-"
    return ",".join(_ABSENT if value is None else _number_to_text(value) for value in values)


def _text_to_optional_numbers(text):
    if text == "-":
        return []
    return [None if token == _ABSENT else _text_to_number(token) for token in text.split(",")]


# The labelled tracks need a separator the comma-joined values do not already use, and a
# second one between a label and its values. Semicolon and colon, not tab: a tab delimiter
# is invisible in every editor and survives exactly until something reformats the file.
def _tracks_to_text(tracks):
    if not tracks:
        return "-"
    return ";".join(f"{label}:{_optional_numbers_to_text(values)}" for label, values in tracks)


def _text_to_tracks(text):
    if text == "-":
        return []
    tracks = []
    for part in text.split(";"):
        label, _, values = part.partition(":")
        tracks.append((label, _text_to_optional_numbers(values)))
    return tracks


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
    for band in data.bands:
        lines.append(" ".join([
            "add_latency_bands",
            band["name"],
            _numbers_to_text(band["times"]),
            _tracks_to_text(band["tracks"]),
            _ABSENT if band["ceiling_ns"] is None else _number_to_text(band["ceiling_ns"]),
            _optional_numbers_to_text(band["breaches"]),
            _optional_numbers_to_text(band["observations"]),
        ]))
    for name, total, rate in data.counters:
        lines.append(" ".join(["add_counter", name, _number_to_text(total), _number_to_text(rate)]))
    for name, value in data.gauges:
        lines.append(" ".join(["add_gauge", name, _number_to_text(value)]))

    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def _replay_add_histogram(data, name, bounds, counts):
    add_histogram(data, name, _text_to_numbers(bounds), _text_to_numbers(counts))


def _replay_add_latency_bands(data, name, times, tracks, ceiling, breaches, observations):
    add_latency_bands(
        data, name, _text_to_numbers(times), _text_to_tracks(tracks),
        None if ceiling == _ABSENT else _text_to_number(ceiling),
        _text_to_optional_numbers(breaches),
        _text_to_optional_numbers(observations),
    )


def _replay_add_counter(data, name, total, rate):
    add_counter(data, name, _text_to_number(total), _text_to_number(rate))


def _replay_add_gauge(data, name, value):
    add_gauge(data, name, _text_to_number(value))


REPLAY_DISPATCH = {
    "set_component": set_component,
    "add_histogram": _replay_add_histogram,
    "add_latency_bands": _replay_add_latency_bands,
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


def format_clock_time(epoch_seconds):
    """Render an epoch second as local wall-clock HH:MM.

    Wall clock rather than time elapsed, because the question this chart gets asked is
    "what was happening at 13:00" -- and an axis counting minutes from an arbitrary window
    start cannot be lined up against a market event, an alarm, or anyone's recollection.
    """
    import time

    return time.strftime("%H:%M", time.localtime(epoch_seconds))


# Lowest percentile palest: the tracks are then read as one distribution widening and
# narrowing rather than as three unrelated lines that happen to share an axis.
BAND_TRACK_COLOURS = ["#8FBCE6", "#4C72B0", "#1F3D63"]
BAND_CEILING_COLOUR = "#C0392B"


def draw_latency_bands(axis, breach_axis, band):
    """Draw percentiles over time, the ceiling, and the count of orders that exceeded it.

    The two axes answer two different questions and are stacked rather than overlaid
    because their units have nothing to do with each other. The upper one is diagnostic --
    how did the distribution move, and when. The lower one is the compliance answer, and
    it is a COUNT, not a percentile: a p99 sitting under the ceiling still permits one
    order in a hundred above it, which over a session is a large number of late orders. A
    chart that showed only the percentile tracks would let a reader conclude "we were
    inside the limit all day" from a picture that cannot support it.
    """
    import matplotlib.ticker as mticker

    times = band["times"]
    tracks = band["tracks"]
    ceiling_ns = band["ceiling_ns"]
    breaches = band["breaches"]

    axis.set_yscale("log")
    axis.set_title(band["name"], fontsize=10, fontweight="bold")

    if not times or not any(any(value is not None for value in values) for _, values in tracks):
        axis.text(0.5, 0.5, "no observations recorded", transform=axis.transAxes,
                  ha="center", va="center", color="0.45", fontsize=9)
        axis.set_title(f"{band['name']} (no observations)", fontsize=10, fontweight="bold")
        return

    # Shade between the outermost tracks first, so the lines sit on top of it. Gaps are
    # segmented rather than bridged: an interval with no traffic must not be spanned by a
    # band implying the venue was measured throughout.
    if len(tracks) >= 2:
        low_values, high_values = tracks[0][1], tracks[-1][1]
        segment_x, segment_low, segment_high = [], [], []
        for index, moment in enumerate(times):
            low, high = low_values[index], high_values[index]
            if low is None or high is None:
                if segment_x:
                    axis.fill_between(segment_x, segment_low, segment_high,
                                      color=BAND_TRACK_COLOURS[1], alpha=0.18, linewidth=0)
                segment_x, segment_low, segment_high = [], [], []
                continue
            segment_x.append(moment)
            segment_low.append(low)
            segment_high.append(high)
        if segment_x:
            axis.fill_between(segment_x, segment_low, segment_high,
                              color=BAND_TRACK_COLOURS[1], alpha=0.18, linewidth=0)

    # 'post', not the default: a percentile derived from the interval beginning at times[i]
    # describes that interval, so it must be held forward across it rather than sloped
    # towards the next point as though the venue had been sampled continuously.
    for index, (label, values) in enumerate(tracks):
        colour = BAND_TRACK_COLOURS[min(index, len(BAND_TRACK_COLOURS) - 1)]
        axis.step(times, [float("nan") if value is None else value for value in values],
                  where="post", color=colour, linewidth=1.4, label=label)

    if ceiling_ns is not None:
        axis.axhline(ceiling_ns, color=BAND_CEILING_COLOUR, linestyle="--", linewidth=1.6)
        axis.annotate(f"ceiling {format_latency_ns(ceiling_ns)}",
                      xy=(times[0], ceiling_ns), xytext=(4, 3), textcoords="offset points",
                      color=BAND_CEILING_COLOUR, fontsize=8, fontweight="bold")

    axis.set_xlim(times[0], times[-1])
    axis.set_ylabel("latency (log scale)")
    axis.grid(True, which="major", axis="both", linestyle=":", alpha=0.3)
    axis.yaxis.set_major_formatter(
        mticker.FuncFormatter(lambda value, _: format_latency_ns(value)))
    axis.yaxis.set_minor_formatter(mticker.NullFormatter())
    axis.legend(fontsize=8, loc="upper left", framealpha=0.9, ncol=len(tracks))

    draw_breach_counts(breach_axis, times, breaches, ceiling_ns, band["observations"])


BAND_VOLUME_COLOUR = "#6B7280"


def draw_breach_counts(axis, times, breaches, ceiling_ns, observations):
    """Draw orders over the ceiling, and how many orders there were, beneath the percentiles.

    The two share this axis because they are both counts per interval, and because they are
    only useful together. Breaches alone cannot distinguish "nothing was late" from "almost
    nothing was sent", and those call for opposite responses.
    """
    import matplotlib.ticker as mticker

    axis.set_xlim(times[0], times[-1])
    axis.set_xlabel("time")
    axis.xaxis.set_major_formatter(
        mticker.FuncFormatter(lambda value, _: format_clock_time(value)))
    axis.tick_params(axis="y", labelsize=8)

    if ceiling_ns is not None and breaches:
        width = (times[1] - times[0]) if len(times) > 1 else 1.0
        heights = [0.0 if value is None else value for value in breaches]
        axis.bar(times, heights, width=width, align="edge",
                 color=BAND_CEILING_COLOUR, alpha=0.85, linewidth=0, label="over ceiling")
        axis.set_ylabel("over ceiling", fontsize=8, color=BAND_CEILING_COLOUR)
        axis.tick_params(axis="y", labelcolor=BAND_CEILING_COLOUR)
        axis.grid(True, axis="y", linestyle=":", alpha=0.3)
        axis.annotate(
            f"{sum(heights):,.0f} orders over {format_latency_ns(ceiling_ns)} in this window",
            xy=(0.995, 0.92), xycoords="axes fraction", ha="right", va="top",
            fontsize=8, color=BAND_CEILING_COLOUR, fontweight="bold")
    else:
        # No ceiling means no breach question was asked; say so rather than showing an
        # empty axis that reads as a measured zero.
        axis.set_yticks([])
        axis.text(0.5, 0.72,
                  "no ceiling given -- pass --ceiling to count orders over the limit",
                  transform=axis.transAxes, ha="center", va="center", color="0.45", fontsize=8)

    if not observations:
        return
    volume_axis = axis.twinx()
    volume_axis.step(times, [0.0 if value is None else value for value in observations],
                     where="post", color=BAND_VOLUME_COLOUR, linewidth=1.0, alpha=0.9)
    volume_axis.set_ylabel("orders", fontsize=8, color=BAND_VOLUME_COLOUR)
    volume_axis.tick_params(axis="y", labelsize=8, labelcolor=BAND_VOLUME_COLOUR)
    # From zero, always. On a volume axis an autoscaled floor turns a 10% dip into a
    # cliff, which is the misreading this line was added to prevent.
    volume_axis.set_ylim(0, max(value for value in observations if value is not None) * 1.15)


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
    if data.histograms or data.bands:
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


def assemble_figure_bands(table_height, chart_height, histogram_height):
    """Lay out the figure's horizontal bands, skipping the kinds this figure has none of.

    Each argument is that band's height in inches, or None if it is not wanted. Returns
    the heights actually used plus each band's row index, so a figure showing only some of
    the four kinds reserves no blank paper where the others would have gone.
    """
    heights, indices = [], []
    for height in (table_height, chart_height, histogram_height):
        if height is None:
            indices.append(None)
            continue
        indices.append(len(heights))
        heights.append(height)
    # A figure with nothing on it still needs one row to hang its title from.
    if not heights:
        heights.append(1.2)
    return heights, indices[0], indices[1], indices[2]


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

    Band charts get a band of their own between the tables and the histograms, one per row
    at full width, each split into a tall percentile axis and a short breach-count axis
    sharing its X. They are not gridded two-up like the histograms: their X axis is a
    trading session, and half a figure width leaves each interval a couple of pixels --
    at which point a five-minute excursion and a single glitch look identical.
    """
    import matplotlib.pyplot as plt

    histograms = data.histograms
    bands = data.bands
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
    # Neither counters nor gauges means no table band at all. Two header-only frames
    # announcing columns with nothing under them read as a fetch that failed, which is a
    # different claim from not having asked for them (--metrics bands, say).
    show_tables = bool(counter_rows or gauge_rows)
    table_height_inches = (
        max(1.2, TABLE_LINE_HEIGHT_INCHES * table_line_count + 0.3) if show_tables else 0.0)
    # No histograms at all is a normal shape -- the matching engine exposes counters only --
    # and it must not reserve a band of blank paper below the tables.
    histogram_height_inches = (4.6 if overlay else 3.6) * rows
    band_height_inches = 4.4 * len(bands)

    # A band chart needs width for its time axis, so its presence sets a floor on the
    # figure width even when there is only one histogram column beside it.
    figure_width_inches = 7.5 * columns
    if bands:
        figure_width_inches = max(figure_width_inches, 13.0)

    band_heights, table_band, chart_band, histogram_band = assemble_figure_bands(
        table_height_inches if show_tables else None,
        band_height_inches if bands else None,
        histogram_height_inches if rows else None,
    )

    figure = plt.figure(figsize=(figure_width_inches, sum(band_heights)))
    if len(band_heights) == 1:
        outer = figure.add_gridspec(1, 1)
    else:
        outer = figure.add_gridspec(
            len(band_heights), 1, height_ratios=band_heights, hspace=0.35 / max(rows, 1),
        )
    # Room for the suptitle reserved in INCHES, not as a fraction of the figure. A fraction
    # that suits a four-panel figure leaves a bands-only one with its title sitting on top
    # of the first panel's, because the two differ in height by a factor of four.
    outer.update(top=1.0 - 0.45 / sum(band_heights))

    # --- summary tables, across the full width ---
    if table_band is not None:
        text_axis = figure.add_subplot(outer[table_band])
        text_axis.axis("off")
        text_axis.text(
            0.0, 1.0, table_text, family="monospace", fontsize=10,
            va="top", ha="left", transform=text_axis.transAxes,
        )

    # --- band charts, full width, percentiles above their breach counts ---
    if chart_band is not None:
        chart_grid = outer[chart_band].subgridspec(len(bands), 1, hspace=0.45)
        for index, band in enumerate(bands):
            pair = chart_grid[index, 0].subgridspec(2, 1, height_ratios=[3.0, 1.0], hspace=0.08)
            percentile_axis = figure.add_subplot(pair[0, 0])
            breach_axis = figure.add_subplot(pair[1, 0], sharex=percentile_axis)
            percentile_axis.tick_params(axis="x", labelbottom=False)
            draw_latency_bands(percentile_axis, breach_axis, band)

    # --- histograms, in a grid beneath ---
    if histogram_band is None:
        figure.suptitle(summary_title(data), fontsize=14, fontweight="bold")
        return figure

    inner = outer[histogram_band].subgridspec(rows, columns, wspace=0.32, hspace=0.55)
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
    root.title(summary_title(data))

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
                        help="comma list of histogram,bands,counter,gauge,all (default: all)")
    parser.add_argument("--ceiling", metavar="DURATION",
                        help="latency a round trip is not supposed to exceed, e.g. 2.5ms, "
                             "500us, or a bare nanosecond count. Drawn on the band chart, and "
                             "orders above it counted EXACTLY -- which needs a bucket bound "
                             "sitting on the same value, or the count is refused rather than "
                             "estimated")
    parser.add_argument("--since", type=int, default=60, metavar="MINUTES",
                        help="how far back the band chart reaches (default: 60)")
    parser.add_argument("--step", type=int, default=30, metavar="SECONDS",
                        help="band chart resolution (default: 30). Below the scrape interval "
                             "the extra points carry no extra information; much above 60 and a "
                             "burst lasting a minute or two is a single point with no shape")
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
    if arguments.step < 1:
        parser.error("--step must be at least 1 second")
    if arguments.since < 1:
        parser.error("--since must be at least 1 minute")
    if arguments.ceiling is not None:
        try:
            parse_duration_ns(arguments.ceiling)
        except ValueError as error:
            parser.error(str(error))
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
    valid = {"histogram", "bands", "counter", "gauge", "all"}
    invalid = requested - valid
    if invalid:
        raise ValueError(f"invalid --metrics value(s): {sorted(invalid)}")
    if "all" in requested:
        return {"histogram", "bands", "counter", "gauge"}
    return requested


DURATION_UNITS_NS = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000}


def parse_duration_ns(text):
    """Parse '2.5ms', '500us', '1s' or a bare nanosecond count into nanoseconds.

    A unit is accepted because the ceiling is a figure someone quotes in milliseconds and
    then has to convert; a bare number is accepted because the configured bucket bounds are
    written in nanoseconds and it must be possible to paste one in unaltered.
    """
    cleaned = text.strip().lower()
    number, multiplier = cleaned, 1
    # Longest unit first, so 'ns'/'us'/'ms' are matched before the 's' they all end with.
    for unit, unit_multiplier in sorted(DURATION_UNITS_NS.items(), key=lambda pair: -len(pair[0])):
        if cleaned.endswith(unit) and len(cleaned) > len(unit):
            number, multiplier = cleaned[: -len(unit)].strip(), unit_multiplier
            break
    # One conversion at the end, covering both the unit and bare forms. Converting inside
    # the loop let a bad number in front of a real unit ('2.5years' -> '2.5year') raise a
    # bare ValueError past this message, and argparse reported it as a float conversion
    # failure naming a string the user never typed.
    try:
        return float(number) * multiplier
    except ValueError:
        raise ValueError(
            f"cannot read '{text}' as a duration: give a bare nanosecond count, "
            f"or a number with one of {', '.join(sorted(DURATION_UNITS_NS))}") from None


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
        data = build_demo_dataset(arguments.component, metrics_requested, arguments.hist, config,
                                  ceiling_ns=(None if arguments.ceiling is None
                                              else parse_duration_ns(arguments.ceiling)))
    else:
        data = fetch_from_prometheus(
            arguments.component, arguments.sample, arguments.prom_url,
            metrics_requested, arguments.hist, arguments.window, config,
            band_options={
                "since_minutes": arguments.since,
                "step_seconds": arguments.step,
                "ceiling_ns": (None if arguments.ceiling is None
                               else parse_duration_ns(arguments.ceiling)),
            },
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
