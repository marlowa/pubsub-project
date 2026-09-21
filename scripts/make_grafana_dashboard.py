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


# A series is put on the right axis by NAMING it "... (right axis)", which the override
# below matches. There is deliberately no parameter for it: the axis has to follow the
# legend text a reader sees, and two ways of saying it could disagree.
# A panel is only as tall as its legend lets it be. The legend is a table underneath the
# chart, one row per series, and Grafana does not grow the panel to fit it: rows past the
# bottom edge are simply not shown. A reader then sees coloured lines with no key, which has
# happened twice on this dashboard and both times looked like a fault in the data rather
# than in the layout. So a panel with more than four series is given the room for them.
#
# This only works where the series can be counted here. A query whose legend is a template,
# such as "{{component}}/{{scope}}", draws one line per thread the venue happens to be running
# and its count is not known until Grafana asks Prometheus. Those panels are given full width
# and read by hovering rather than by the legend.
def height_for(series_count, base=11):
    """Grid height that leaves the legend room for every series.

    @param[in] series_count How many named series the panel draws.
    @param[in] base         Height for a panel of up to four series.
    """
    return base + max(0, series_count - 4)


def ts(title, targets, w=12, h=11, unit="ns", desc=""):
    return {
        "type": "timeseries", "title": title, "description": desc,
        "gridPos": {"h": h, "w": w, "x": 0, "y": 0}, "datasource": DS, "targets": targets,
        "fieldConfig": {"defaults": {
            "unit": unit, "min": 0,
            # Gaps are drawn as gaps. Grafana's default bridges them, which asserts we
            # measured something during a period when we measured nothing.
            "custom": {"spanNulls": False, "lineWidth": 1, "fillOpacity": 5, "showPoints": "never"},
        }, "overrides": [
            # Series whose name ends "(right axis)" are a different quantity from the rest of
            # the panel and must not share a scale with them.
            {"matcher": {"id": "byRegexp", "options": ".*\\(right axis\\)$"},
             "properties": [{"id": "custom.axisPlacement", "value": "right"},
                            {"id": "unit", "value": "reqps"},
                            {"id": "custom.lineStyle", "value": {"dash": [8, 4], "fill": "dash"}},
                            {"id": "custom.fillOpacity", "value": 0}]},
        ]},
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
    [tgt(f'histogram_quantile(0.90, sum by (le, component, scope) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}])))',
         "{{component}}/{{scope}} p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component, scope) (rate(itc_queue_latency_nanoseconds_bucket{{{ONE}}}[{RATE}])))',
         "{{component}}/{{scope}} p99", "B")],
    w=24,
    desc="Watch this across the quiet phases of a trading-day run. A thread that sleeps "
         "between messages has to be woken for each one, and the cost of that appears here "
         "and nowhere else. Full width because it has one line per thread, and the legend is "
         "what makes it readable."))

# --- the round trip, and its one instrumented interior segment ---
panels.append(heat(
    "Order round trip — $component — distribution over time",
    f'sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{ONE}}}[{RATE}]))',
    desc="Order off the client connection to starting to send its ExecutionReport."))

# One percentile per panel, and deliberately so.
#
# When both p90 and p99 for every gateway shared one chart, there were four lines on it and
# four names in the legend for what a reader thinks of as two things. The eye then compares
# the highest line against the lowest and concludes that one gateway is slower than the other,
# when what it has actually compared is one gateway's p99 against the other's p90. Splitting
# them means every line on a panel is the same percentile, so the vertical distance between
# two lines is a difference between gateways and nothing else.
ROUND_TRIP_ESTIMATE_NOTE = ("histogram_quantile interpolates inside whichever bucket the rank falls in, so these "
                            "are estimates whose accuracy is a property of the configured bucket bounds.")

panels.append(ts(
    "Order round trip — p90, all gateways compared",
    [tgt(f'histogram_quantile(0.90, sum by (le, component) (rate(order_round_trip_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}}", "A")],
    desc="One line per gateway, all of them the ninetieth percentile, so a line above another "
         "means that gateway really is slower. " + ROUND_TRIP_ESTIMATE_NOTE))

panels.append(ts(
    "Order round trip — p99, all gateways compared",
    [tgt(f'histogram_quantile(0.99, sum by (le, component) (rate(order_round_trip_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}}", "A")],
    desc="The same gateways at the ninety-ninth percentile, on its own scale. The tail is "
         "several times the typical figure, so plotting it beside p90 flattens p90 into a line "
         "along the bottom where differences between gateways cannot be seen. "
         + ROUND_TRIP_ESTIMATE_NOTE))

panels.append(ts(
    "WAL append — percentiles, by sequencer",
    [tgt(f'histogram_quantile(0.90, sum by (le, component) (rate(wal_append_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component) (rate(wal_append_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p99", "B")],
    w=24,
    desc="Committing one record to the write-ahead log. The metric on which the lazytime "
                 "mount option showed up. Two percentiles share this chart because only one "
                 "sequencer is ever the leader, so there is only ever one pair of lines on it "
                 "and there is nothing to confuse them with."))

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
    "Protocol comparison — round trip p90/p99, with the rate each was measured at",
    [tgt(f'histogram_quantile(0.90, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p99", "B"),
     tgt(f'histogram_quantile(0.90, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p90", "C"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_round_trip_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p99", "D"),
     # The rates share this panel deliberately. A latency compared against a latency measured
     # at a different offered rate says nothing about the protocols: the busier one carries
     # more queueing and looks worse for reasons that have nothing to do with its encoding.
     # Putting the rates anywhere else invites exactly that reading, so they are here, on a
     # second axis, where the comparison cannot be seen without them.
     tgt(f'sum(rate(order_round_trip_nanoseconds_count{{{FIX}}}[{RATE}]))', "FIX rate (right axis)", "E"),
     tgt(f'sum(rate(order_round_trip_nanoseconds_count{{{BIN}}}[{RATE}]))', "binary rate (right axis)", "F")],
    w=24, h=height_for(6),
    desc="ONLY MEANINGFUL WHEN THE TWO RATE LINES COINCIDE. Run dashboard_load.py --mode "
         "compare, which drives both protocols at identical rates in lockstep. The default "
         "interfere mode deliberately runs them at different rates -- it answers a different "
         "question -- and a latency comparison taken from it is measuring the offered load, "
         "not the protocol. Both instances of each protocol are pooled, because the "
         "comparison is between protocols rather than between instances."))

# The decode-and-forward panel is the one that can actually answer "which protocol is
# faster", and it goes next to the round-trip comparison so the two are read together.
# Measured 2026-09-19: the round trips differed by 57us inside 900us and the inter-thread
# queue latencies by 1us, because everything after the gateway is the same code for both.
panels.append(ts(
    "Protocol comparison — gateway decode and forward p90/p99 (the part that differs)",
    [tgt(f'histogram_quantile(0.90, sum by (le) (rate(order_ingress_to_forward_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_ingress_to_forward_nanoseconds_bucket{{{FIX}}}[{RATE}])))', "FIX p99", "B"),
     tgt(f'histogram_quantile(0.90, sum by (le) (rate(order_ingress_to_forward_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p90", "C"),
     tgt(f'histogram_quantile(0.99, sum by (le) (rate(order_ingress_to_forward_nanoseconds_bucket{{{BIN}}}[{RATE}])))', "binary p99", "D"),
     # Carried for the same reason as on the round-trip panel: a decode time is still a
     # latency, and a gateway being given more orders per second can queue behind itself.
     tgt(f'sum(rate(order_ingress_to_forward_nanoseconds_count{{{FIX}}}[{RATE}]))', "FIX rate (right axis)", "E"),
     tgt(f'sum(rate(order_ingress_to_forward_nanoseconds_count{{{BIN}}}[{RATE}]))', "binary rate (right axis)", "F")],
    w=24, h=height_for(6),
    desc="Parsing, validating and building the envelope -- the work that is the gateway's "
         "own. THIS is where FIX and binary differ; the round-trip panel above spans the "
         "sequencer, matching engine and report path, which are identical code for both, so "
         "it shows that they differ without showing where. Still read the rate lines: a "
         "decode time is a latency and a busier gateway can queue behind itself."))

# Where an order's time actually goes, stage by stage.
#
# Every component records how long an order had already been inside the venue when it reached
# that component, all counted from the moment a gateway read it off the client connection. So
# these lines are cumulative and they only ever go up along the path: the useful reading is the
# GAP between two neighbouring lines, which is the time spent in between. A line on its own
# says how far along the journey that point sits, which is a fact about the venue's shape and
# not about its speed.
#
# Ordered here as an order meets them, so the chart is read from bottom to top.
panels.append(ts(
    "Where the time goes — how far through the venue an order is at each point (median)",
    [tgt('histogram_quantile(0.50, sum by (le) (rate(order_path_elapsed_nanoseconds_bucket'
         '{application="pubsub", component=~"sequencer.*", scope="order_in"}[' + RATE + '])))', "1. reached the sequencer", "A"),
     tgt('histogram_quantile(0.50, sum by (le) (rate(order_path_elapsed_nanoseconds_bucket'
         '{application="pubsub", component=~"sequencer.*", scope="order_out"}[' + RATE + '])))', "2. sequencer sent it on", "B"),
     tgt('histogram_quantile(0.50, sum by (le) (rate(order_path_elapsed_nanoseconds_bucket'
         '{application="pubsub", component=~"matching_engine.*", scope="order_in"}[' + RATE + '])))', "3. reached the matching engine", "C"),
     tgt('histogram_quantile(0.50, sum by (le) (rate(order_path_elapsed_nanoseconds_bucket'
         '{application="pubsub", component=~"matching_engine.*", scope="er_out"}[' + RATE + '])))', "4. matched, report sent", "D"),
     tgt('histogram_quantile(0.50, sum by (le) (rate(order_path_elapsed_nanoseconds_bucket'
         '{application="pubsub", component=~"sequencer.*", scope="er_in"}[' + RATE + '])))', "5. report back at the sequencer", "E"),
     tgt('histogram_quantile(0.50, sum by (le) (rate(order_path_elapsed_nanoseconds_bucket'
         '{application="pubsub", component=~"sequencer.*", scope="er_out"}[' + RATE + '])))', "6. sequencer sent the report on", "F"),
     tgt('histogram_quantile(0.50, sum by (le) (rate(order_path_elapsed_nanoseconds_bucket'
         '{application="pubsub", component=~".*order_gateway.*", scope="er_in"}[' + RATE + '])))', "7. report back at the gateway", "G"),
     tgt('histogram_quantile(0.50, sum by (le) (rate(order_round_trip_nanoseconds_bucket'
         '{application="pubsub"}[' + RATE + '])))', "8. report sent to the member", "H")],
    w=24, h=height_for(8),
    desc="Read the GAPS, not the lines. Each line is how much of the round trip had already "
         "gone by the time an order reached that point, so the distance between two of them is "
         "the time spent in between. The topmost line is the round trip itself, and the legs "
         "below it should add up to it -- if they stop adding up, a measurement point is in "
         "the wrong place."))

# Waiting and queueing are different problems with opposite remedies, and the wait alone
# cannot tell them apart. Both are recorded over exactly the same messages so they can be
# read against each other on one chart.
panels.append(ts(
    "Waiting for a thread, or waiting in a queue — $component",
    [tgt('histogram_quantile(0.90, sum by (le) (rate(itc_queue_latency_nanoseconds_bucket'
         '{application="pubsub", component="$component"}[' + RATE + '])))', "wait p90", "A"),
     tgt('histogram_quantile(0.99, sum by (le) (rate(itc_queue_latency_nanoseconds_bucket'
         '{application="pubsub", component="$component"}[' + RATE + '])))', "wait p99", "B"),
     tgt('histogram_quantile(0.90, sum by (le) (rate(itc_queue_depth_bucket'
         '{application="pubsub", component="$component"}[' + RATE + '])))', "messages behind it, p90 (right axis)", "C")],
    w=24,
    desc="A message that waited a long time behind an EMPTY queue was waiting for its thread "
         "to be scheduled. One that waited a long time behind a hundred others was waiting its "
         "turn. Those call for opposite remedies -- give the thread a core, or give the work "
         "less to do -- and the waiting time on its own cannot tell you which."))

panels.append(ts(
    "Gateway decode and forward — percentiles by gateway instance",
    [tgt(f'histogram_quantile(0.90, sum by (le, component) (rate(order_ingress_to_forward_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p90", "A"),
     tgt(f'histogram_quantile(0.99, sum by (le, component) (rate(order_ingress_to_forward_nanoseconds_bucket{{{APP}}}[{RATE}])))', "{{component}} p99", "B")],
    desc="The same measurement per process rather than pooled by protocol. Two instances of "
         "one protocol disagreeing is a property of those processes -- what they are pinned "
         "to, what else they are carrying -- rather than of the encoding."))

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
    # Six hours, because almost every panel on this dashboard plots a percentile of a RATE, and a
    # rate is zero whenever no orders are flowing. A percentile of a distribution with nothing in
    # it is undefined, so Grafana draws nothing at all rather than a line along the bottom. The
    # dashboard therefore looks broken during any period when the venue is simply idle, which is
    # most of the time: load runs last half an hour and the venue sits up for the rest of the day.
    #
    # Half an hour showed only a run that was happening at the moment the dashboard was opened.
    # Six hours reaches back over a working session, so a run finished earlier is still on the
    # chart when you come to look at it. If a run is older than that, set the picker to the run's
    # own start and end time -- the measurements are kept for ninety days and none of this loses
    # them, it only decides what is on screen when the page first loads.
    "time": {"from": "now-6h", "to": "now"},
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
