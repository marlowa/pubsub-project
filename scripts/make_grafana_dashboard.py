#!/usr/bin/env python3
"""Generate the venue's Grafana dashboard as JSON.

    python3 scripts/make_grafana_dashboard.py grafana/dashboards/pubsub-venue-latency.json

A dashboard clicked together in a browser lives in one Grafana instance's database and
leaves with whoever built it. Generated from this script it is a file: reviewable, diffable,
version controlled, and importable into any other Grafana -- which is the only form in which
it can be handed to someone who has access to an environment you do not.

Generated rather than hand-written because the JSON is several hundred lines of deeply
nested panel definitions, where a misplaced brace produces a dashboard that loads and
silently shows nothing.

Two conventions the panels follow, both learned the hard way:

  * Every panel names the executable it describes, through the `component` template
    variable or an explicit legend. A chart that aggregates anonymously across components
    can report that order placement is slow but not which component to fix, and fixes are
    made component by component.

  * Heatmaps resolve to exactly one series source. A heatmap collapses every series into a
    single coloured surface, so four components merge into a picture indistinguishable from
    one component's -- with no visual cue that it happened. That is worse than useless: it
    looks authoritative and describes a population that does not exist.
"""
import argparse
import json

DS = {"type": "prometheus", "uid": "pubsub-prom"}
APP = 'application="pubsub"'
# Every panel is filtered to one executable. Aggregating across components produces a
# chart that can say order placement is slow but not which component to fix, and fixes
# are made component by component.
ONE = 'application="pubsub", component="$component"'
RATE = "$__rate_interval"


def tgt(expr, legend, ref="A", fmt=None):
    t = {"refId": ref, "datasource": DS, "expr": expr, "legendFormat": legend, "editorMode": "code", "range": True}
    if fmt:
        t["format"] = fmt
    return t


def ts(title, targets, x, y, w=12, h=8, unit="ns", desc=""):
    return {
        "type": "timeseries", "title": title, "description": desc,
        "gridPos": {"h": h, "w": w, "x": x, "y": y}, "datasource": DS, "targets": targets,
        "fieldConfig": {"defaults": {
            "unit": unit, "min": 0,
            # Gaps are drawn as gaps. Grafana's default bridges them, which asserts we
            # measured something during a period when we measured nothing.
            "custom": {"spanNulls": False, "lineWidth": 1, "fillOpacity": 5, "showPoints": "never"},
        }, "overrides": []},
        "options": {"legend": {"displayMode": "table", "placement": "bottom", "calcs": ["mean", "max"]},
                    "tooltip": {"mode": "multi", "sort": "desc"}},
    }


def heat(title, expr, x, y, w=24, h=9, desc=""):
    return {
        "type": "heatmap", "title": title, "description": desc,
        "gridPos": {"h": h, "w": w, "x": x, "y": y}, "datasource": DS,
        "targets": [tgt(expr, "{{le}}", fmt="heatmap")],
        "options": {
            "calculate": False,          # Prometheus already bucketed this; recalculating gives nonsense
            "cellGap": 1,
            "color": {"mode": "scheme", "scheme": "Turbo", "steps": 64, "exponent": 0.5, "reverse": False},
            "yAxis": {"unit": "ns", "axisPlacement": "left"},
            "legend": {"show": True}, "tooltip": {"mode": "single", "yHistogram": True},
            "filterValues": {"le": 1e-9},
        },
    }


def stat(title, targets, x, y, w=12, h=8, unit="short", desc=""):
    return {
        "type": "stat", "title": title, "description": desc,
        "gridPos": {"h": h, "w": w, "x": x, "y": y}, "datasource": DS, "targets": targets,
        "fieldConfig": {"defaults": {"unit": unit, "color": {"mode": "thresholds"},
                                     "thresholds": {"mode": "absolute",
                                                    "steps": [{"color": "green", "value": None},
                                                              {"color": "red", "value": 1}]}},
                        "overrides": []},
        "options": {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
                    "textMode": "auto", "colorMode": "value", "graphMode": "area"},
    }



# One executable at a time. A dashboard that overlays a gateway's pool on a matching
# engine's invites a comparison between things that have nothing to do with each other;
# the picker makes the selection explicit instead. framework_pdu_messages_total is the
# label source because every thread that names a metrics_scope has one.
TEMPLATING = {
    "list": [
        {
            "name": "component", "label": "component", "type": "query", "datasource": DS,
            "query": {"query": 'label_values(framework_pdu_messages_total{application="pubsub"}, component)', "refId": "A"},
            "refresh": 2, "includeAll": False, "multi": False, "sort": 1, "current": {},
        },
        {
            # Chained off component. Each pool is sized independently in configuration, so a
            # pool problem is only actionable once you know WHICH pool -- the fix is a change
            # to that pool's own initial_pools / objects_per_pool. Summing across pools, or
            # assuming there is only ever one, hides the thing you would act on.
            "name": "pool", "label": "pool", "type": "query", "datasource": DS,
            "query": {"query": 'label_values(pool_bytes_reserved{application="pubsub", component="$component"}, scope)', "refId": "A"},
            "refresh": 2, "includeAll": False, "multi": False, "sort": 1, "current": {},
        },
    ]
}

panels = []

# --- the ITC hand-off: the new instrumentation, and the point of the exercise ---
panels.append(heat(
    "ITC hop — $component — queue wait + wakeup",
    f'sum by (le) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}]))', 0, 0,
    desc="Time from a message being enqueued to it being dispatched: the queue wait plus the "
         "wakeup (the eventfd write and the epoll_wait return). Message construction happens "
         "before the stamp, so none of it is in here."))

panels.append(ts(
    "ITC hop — $component — percentiles by thread",
    [tgt(f'histogram_quantile(0.50, sum by (le, component, scope) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}])))', "{{component}}/{{scope}} p50", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component, scope) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}])))', "{{component}}/{{scope}} p99", "B")],
    0, 9, desc="Watch this across the quiet phases of a trading-day run. A thread that sleeps "
               "between messages has to be woken for each one, and the cost of that appears here "
               "and nowhere else."))

panels.append(stat(
    "ITC samples excluded (unstamped) — $component",
    [tgt(f'increase(itc_queue_latency_unstamped_total{{{ONE}}}[$__range])', "unstamped", "A")],
    12, 9, unit="short",
    desc="Messages dispatched with no enqueue stamp over the window in view, and therefore NOT in "
         "the histogram above. "
         "Anything other than zero means the distribution is describing a subset of the traffic "
         "while looking complete. Green is zero."))

# --- the round trip, and its one instrumented interior segment ---
panels.append(heat(
    "Order round trip — $component — distribution over time",
    f'sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{ONE}}}[{RATE}]))', 0, 17,
    desc="Order off the client connection to starting to send its ExecutionReport."))

panels.append(ts(
    "Order round trip — percentiles, all gateways compared",
    [tgt(f'histogram_quantile(0.50, sum by (le, component) (rate(order_round_trip_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p50", "A"),
     tgt(f'histogram_quantile(0.90, sum by (le, component) (rate(order_round_trip_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p90", "B"),
     tgt(f'histogram_quantile(0.99, sum by (le, component) (rate(order_round_trip_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p99", "C")],
    0, 26, desc="histogram_quantile interpolates inside the bucket the rank falls in, so these are "
                "estimates whose quality is a property of the configured bucket bounds."))

panels.append(ts(
    "WAL append — percentiles, by sequencer",
    [tgt(f'histogram_quantile(0.50, sum by (le, component) (rate(wal_append_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p50", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component) (rate(wal_append_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p99", "B")],
    12, 26, desc="Committing one record to the write-ahead log. The metric on which the lazytime "
                 "mount option showed up."))

# --- protocol comparison: the one case where two executables belong on one chart ---
#
# Not an exception to "name the executable" so much as the point of it. These are the same
# metric, on identical bucket bounds, measured at the same place in two gateways doing the
# same job by different protocols -- which is the question the metric was created to answer.
# Each series names what it is, so nothing is anonymous; what is aggregated is instances of
# one protocol, which are like with like.
FIX = 'application="pubsub", component=~"fix_order_gateway.*"'
BIN = 'application="pubsub", component=~"binary_order_gateway.*"'

panels.append(ts(
    "Protocol comparison — order round trip, FIX against binary",
    [tgt(f'histogram_quantile(0.50, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p50", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p99", "B"),
     tgt(f'histogram_quantile(0.50, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p50", "C"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p99", "D")],
    0, 50,
    desc="Both instances of each protocol are pooled, because the comparison is between "
         "protocols rather than between instances -- the per-gateway panel above is where an "
         "individual instance is judged. Reads as nothing until both gateways are taking "
         "orders; today only the binary side has a load generator."))

panels.append(ts(
    "Protocol mix — orders per second by protocol",
    [tgt(f'sum(rate(order_round_trip_nanoseconds_count{{{FIX}}}[{RATE}]))', "FIX", "A"),
     tgt(f'sum(rate(order_round_trip_nanoseconds_count{{{BIN}}}[{RATE}]))', "binary", "B")],
    12, 50, unit="reqps",
    desc="How the traffic divides between the two protocols. Read alongside the panel beside "
         "it: a shift toward one protocol is only evidence about performance if that protocol "
         "is actually the faster one under the load it is being given."))

# --- context: a latency figure without its rate is uninterpretable ---
panels.append(ts(
    "Throughput",
    [tgt(f'rate(orders_processed_total{{{APP}}}[{RATE}])', "{{component}} matched/s", "A"),
     tgt(f'sum by (component) (rate(order_round_trip_nanoseconds_count{{{APP}}}[{RATE}]))', "{{component}} round trips/s", "B")],
    0, 34, unit="reqps",
    desc="The trading-day phases should be legible here: bursts at the open and close, trickles "
         "between, and two stretches of genuine quiet."))

panels.append(ts(
    "Order book — $component",
    [tgt(f'order_book_entries{{{ONE}}}', "{{component}} entries", "A"),
     tgt(f'order_book_migrating{{{ONE}}}', "{{component}} migrating", "B")],
    12, 34, unit="short",
    desc="Orders resting in the book, with the incremental-rehash migration flag overlaid."))

panels.append(ts(
    "Pool occupancy — $component / $pool",
    [tgt(f'pool_objects_allocated{{{APP}, component="$component", scope="$pool"}}', "objects allocated", "A"),
     tgt(f'pool_objects_available{{{APP}, component="$component", scope="$pool"}}', "objects available", "B"),
     tgt(f'pool_objects_per_pool{{{APP}, component="$component", scope="$pool"}}', "capacity per pool", "C")],
    0, 42, unit="short",
    desc="One pool in one process. Headroom is the gap between allocated and available: a pool "
         "that runs it down to nothing is about to chain another, which is what the panel beside "
         "this one records."))

panels.append(ts(
    "Pool pressure — $component / $pool",
    # increase(), not the raw value: these three are cumulative even though the framework
    # registers them as gauges, so plotted raw they only ever rise and five expansions in one
    # second look the same as five across an hour. What is actionable is WHEN a pool was
    # exhausted, so it can be lined up against the load that exhausted it.
    #
    # pool_chain_full is left raw on purpose -- it is how many pools are full right now, a
    # genuine point-in-time reading, and increase() over it would mean nothing.
    [tgt(f'increase(pool_expansion_events{{{APP}, component="$component", scope="$pool"}}[{RATE}])', "expansions", "A"),
     tgt(f'increase(pool_allocation_failures{{{APP}, component="$component", scope="$pool"}}[{RATE}])', "allocation failures", "B"),
     tgt(f'increase(pool_slow_path_allocations{{{APP}, component="$component", scope="$pool"}}[{RATE}])', "slow path allocations", "C"),
     tgt(f'pool_chain_full{{{APP}, component="$component", scope="$pool"}}', "pools full now", "D")],
    12, 42, unit="short",
    desc="The signals that say this pool is sized wrong, and which pool to resize. Expansion "
         "events mean it exhausted its initial pools and had to chain another -- on the "
         "allocation path, under a mutex. Allocation failures mean chaining did not save it. "
         "Both should be flat at zero in a venue whose pools are configured for its load."))

dashboard = {
    "uid": "pubsub-venue-latency",
    "title": "pubsub venue — latency and throughput",
    "tags": ["pubsub", "latency"],
    "timezone": "browser", "schemaVersion": 39, "version": 0,
    "refresh": "10s", "id": None,
    "time": {"from": "now-30m", "to": "now"},
    "panels": panels,
    "templating": TEMPLATING,
}
parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("output", nargs="?", default="grafana/dashboards/pubsub-venue-latency.json",
                    help="where to write the dashboard JSON (default: %(default)s)")
arguments = parser.parse_args()

with open(arguments.output, "w", encoding="utf-8") as handle:
    json.dump(dashboard, handle, indent=2)
print(f"wrote {arguments.output} ({len(panels)} panels)")
