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


def ts(title, targets, w=12, h=11, unit="ns", desc=""):
    return {
        "type": "timeseries", "title": title, "description": desc,
        "gridPos": {"h": h, "w": w, "x": 0, "y": 0}, "datasource": DS, "targets": targets,
        "fieldConfig": {"defaults": {
            "unit": unit, "min": 0,
            # Gaps are drawn as gaps. Grafana's default bridges them, which asserts we
            # measured something during a period when we measured nothing.
            "custom": {"spanNulls": False, "lineWidth": 1, "fillOpacity": 5, "showPoints": "never"},
        }, "overrides": []},
        "options": {"legend": {"displayMode": "table", "placement": "bottom", "calcs": ["mean", "max"]},
                    "tooltip": {"mode": "multi", "sort": "desc"}},
    }


def heat(title, expr, w=24, h=10, desc=""):
    return {
        "type": "heatmap", "title": title, "description": desc,
        "gridPos": {"h": h, "w": w, "x": 0, "y": 0}, "datasource": DS,
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


def stat(title, targets, w=12, h=11, unit="short", desc=""):
    return {
        "type": "stat", "title": title, "description": desc,
        "gridPos": {"h": h, "w": w, "x": 0, "y": 0}, "datasource": DS, "targets": targets,
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
    f'sum by (le) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}]))',
    desc="Time from a message being enqueued to it being dispatched: the queue wait plus the "
         "wakeup (the eventfd write and the epoll_wait return). Message construction happens "
         "before the stamp, so none of it is in here."))

panels.append(ts(
    "ITC hop — $component — percentiles by thread",
    [tgt(f'histogram_quantile(0.90, sum by (le, component, scope) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}])))', "{{component}}/{{scope}} p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component, scope) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}])))', "{{component}}/{{scope}} p99", "B")],
    desc="Watch this across the quiet phases of a trading-day run. A thread that sleeps "
               "between messages has to be woken for each one, and the cost of that appears here "
               "and nowhere else."))

panels.append(stat(
    "ITC samples excluded (unstamped) — $component",
    [tgt(f'increase(itc_queue_latency_unstamped_total{{{ONE}}}[$__range])', "unstamped", "A")],
    unit="short",
    desc="Messages dispatched with no enqueue stamp over the window in view, and therefore NOT in "
         "the histogram above. "
         "Anything other than zero means the distribution is describing a subset of the traffic "
         "while looking complete. Green is zero."))

# --- the round trip, and its one instrumented interior segment ---
panels.append(heat(
    "Order round trip — $component — distribution over time",
    f'sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{ONE}}}[{RATE}]))',
    desc="Order off the client connection to starting to send its ExecutionReport."))

panels.append(ts(
    "Order round trip — percentiles, all gateways compared",
    [tgt(f'histogram_quantile(0.90, sum by (le, component) (rate(order_round_trip_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component) (rate(order_round_trip_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p99", "B")],
    desc="histogram_quantile interpolates inside the bucket the rank falls in, so these are "
                "estimates whose quality is a property of the configured bucket bounds."))

panels.append(ts(
    "WAL append — percentiles, by sequencer",
    [tgt(f'histogram_quantile(0.90, sum by (le, component) (rate(wal_append_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component) (rate(wal_append_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p99", "B")],
    desc="Committing one record to the write-ahead log. The metric on which the lazytime "
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
    [tgt(f'histogram_quantile(0.90, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p99", "B"),
     tgt(f'histogram_quantile(0.90, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p90", "C"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p99", "D")],
    desc="p90 and p99 rather than a median: a latency problem lives in the tail, and a p50 "
         "that barely moves while p99 triples is the case this panel exists to show. Both instances of each protocol are pooled, because the comparison is between "
         "protocols rather than between instances -- the per-gateway panel above is where an "
         "individual instance is judged. Reads as nothing until both gateways are taking "
         "orders; today only the binary side has a load generator."))

panels.append(ts(
    "Protocol mix — orders per second by protocol",
    [tgt(f'sum(rate(order_round_trip_nanoseconds_count{{{FIX}}}[{RATE}]))', "FIX", "A"),
     tgt(f'sum(rate(order_round_trip_nanoseconds_count{{{BIN}}}[{RATE}]))', "binary", "B")],
    unit="reqps",
    desc="How the traffic divides between the two protocols. Read alongside the panel beside "
         "it: a shift toward one protocol is only evidence about performance if that protocol "
         "is actually the faster one under the load it is being given."))

# --- context: a latency figure without its rate is uninterpretable ---
panels.append(ts(
    "Throughput",
    [tgt(f'rate(orders_processed_total{{{APP}}}[{RATE}])', "{{component}} matched/s", "A"),
     tgt(f'sum by (component) (rate(order_round_trip_nanoseconds_count{{{APP}}}[{RATE}]))', "{{component}} round trips/s", "B")],
    unit="reqps",
    desc="The trading-day phases should be legible here: bursts at the open and close, trickles "
         "between, and two stretches of genuine quiet."))

panels.append(ts(
    "Order book — $component",
    [tgt(f'order_book_entries{{{ONE}}}', "{{component}} entries", "A"),
     tgt(f'order_book_migrating{{{ONE}}}', "{{component}} migrating", "B")],
    unit="short",
    desc="Orders resting in the book, with the incremental-rehash migration flag overlaid."))

panels.append(ts(
    "Pool occupancy — $component / $pool",
    [tgt(f'pool_objects_allocated{{{APP}, component="$component", scope="$pool"}}', "objects allocated", "A"),
     tgt(f'pool_objects_available{{{APP}, component="$component", scope="$pool"}}', "objects available", "B"),
     tgt(f'pool_objects_per_pool{{{APP}, component="$component", scope="$pool"}}', "capacity per pool", "C")],
    unit="short",
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
    unit="short",
    desc="The signals that say this pool is sized wrong, and which pool to resize. Expansion "
         "events mean it exhausted its initial pools and had to chain another -- on the "
         "allocation path, under a mutex. Allocation failures mean chaining did not save it. "
         "Both should be flat at zero in a venue whose pools are configured for its load."))

def spacer(height=4):
    """An empty, transparent panel closing the dashboard.

    Without it the last panel sits flush against the bottom edge, which reads as though the
    page has been cut off and there is more below that cannot be reached. A little blank
    space says the dashboard ends here on purpose. Grafana has no padding setting, so the
    space has to be a panel.
    """
    return {"type": "text", "title": "", "transparent": True,
            "gridPos": {"h": height, "w": 24, "x": 0, "y": 0},
            "options": {"mode": "markdown", "content": ""}}


def lay_out(panel_list):
    """Assign gridPos left to right, wrapping to a new row when the width is used up.

    Computed rather than written down because the panels are not a fixed set and their
    heights are not fixed either. Hand-placed coordinates go stale the moment a panel is
    added or made taller, and the symptom is subtle: Grafana silently overlaps or clips
    rather than complaining, and a legend loses its last row off the bottom edge.
    """
    row_top, row_used, row_height = 0, 0, 0
    for panel in panel_list:
        width = panel["gridPos"]["w"]
        if row_used + width > 24:
            row_top += row_height
            row_used, row_height = 0, 0
        panel["gridPos"]["x"] = row_used
        panel["gridPos"]["y"] = row_top
        row_used += width
        row_height = max(row_height, panel["gridPos"]["h"])
    return panel_list


dashboard = {
    "uid": "pubsub-venue-latency",
    "title": "pubsub venue — latency and throughput",
    "tags": ["pubsub", "latency"],
    "timezone": "browser", "schemaVersion": 39, "version": 0,
    "refresh": "10s", "id": None,
    "time": {"from": "now-30m", "to": "now"},
    "panels": lay_out(panels + [spacer()]),
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
