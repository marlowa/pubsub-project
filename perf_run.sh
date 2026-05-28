#!/usr/bin/env bash
# perf_run.sh — start the full FIX sequencer system under perf, fire 1000 NOS
#               via fix8, SIGTERM all processes, and produce per-process perf
#               reports and flamegraph SVGs.
#
# Usage (from the project root):
#   ./perf_run.sh                     # uses build/installed
#   ./perf_run.sh <install_prefix>
#
# Output directory:
#   <prefix>/perf/<YYYYMMDD_HHMMSS>/
#     <name>.perf.data     raw perf samples (one per process)
#     <name>.perf.stderr   perf record stderr (sanity check)
#     <name>.svg           flamegraph SVG (requires FlameGraph scripts)
#     report.txt           combined perf report (--stdio flat profile)
#
# Note: the release build does not use -fno-omit-frame-pointer, so
# CALLGRAPH defaults to "dwarf" (DWARF-based stack unwinding, always works).
# For lower overhead, add -fno-omit-frame-pointer to the Release compile
# options in CMakeLists.txt and change CALLGRAPH to "fp" below.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${1:-$SCRIPT_DIR/build/installed}"
PREFIX="$(realpath "$PREFIX")"
BIN_DIR="$PREFIX/bin"
ETC_DIR="$PREFIX/etc"
LOG_DIR="$PREFIX/log"

FIX8_DIR="/home/marlowa/mystuff/fix8_install"
FIX8_BIN="$FIX8_DIR/bin/f8test"
FIX8_CFG="myfix_gateway_client.xml"
FLAMEGRAPH_DIR="/home/marlowa/mystuff/FlameGraph"

TS="$(date +%Y%m%d_%H%M%S)"
PERF_DIR="$PREFIX/perf/$TS"

STARTUP_DELAY=1   # seconds between app launches (mirrors start_fix_seq_system.py)
SETTLE_TIME=3     # seconds after last app before attaching perf
POST_FIX8_WAIT=2  # seconds after fix8 exits before sending SIGTERM
CALLGRAPH="dwarf" # dwarf always works; fp needs -fno-omit-frame-pointer
FREQ=99           # perf sample frequency in Hz

export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

declare -a APP_NAMES=()
declare -a APP_PIDS=()
declare -a PERF_PIDS=()

# ------------------------------------------------------------------------------
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*" >&2; exit 1; }

# ------------------------------------------------------------------------------
launch_app() {
    local name="$1" bin="$2" config="$3"
    [[ -f "$config" ]] || die "config not found: $config"
    log "Starting $name ..."
    "$BIN_DIR/$bin" "$LOG_DIR/${name}.log" "$config" &
    local pid=$!
    APP_NAMES+=("$name")
    APP_PIDS+=("$pid")
    log "  $name PID $pid"
}

attach_perf() {
    local name="$1" pid="$2"
    local out="$PERF_DIR/${name}.perf.data"
    perf record -p "$pid" -o "$out" \
        --call-graph "$CALLGRAPH" -F "$FREQ" \
        2>"$PERF_DIR/${name}.perf.stderr" &
    PERF_PIDS+=($!)
    log "  perf → $name (PID $pid) → $(basename "$out")"
}

shutdown_apps() {
    log "Sending SIGTERM to all applications ..."
    local pid
    for pid in "${APP_PIDS[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    local i
    for i in "${!APP_PIDS[@]}"; do
        if wait "${APP_PIDS[$i]}" 2>/dev/null; then
            log "  ${APP_NAMES[$i]} exited cleanly"
        else
            log "  ${APP_NAMES[$i]} exited (non-zero status)"
        fi
    done
}

wait_for_perf() {
    log "Waiting for perf to finish writing data ..."
    local pid
    for pid in "${PERF_PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
}

generate_reports() {
    local report="$PERF_DIR/report.txt"
    log "Generating reports in $PERF_DIR ..."
    : > "$report"

    local name
    for name in "${APP_NAMES[@]}"; do
        local data="$PERF_DIR/${name}.perf.data"
        if [[ ! -f "$data" ]]; then
            log "  WARNING: no perf data for $name — skipping"
            continue
        fi

        # Text report (appended to combined file and echoed to terminal)
        {
            printf '======================================================================\n'
            printf '  %s\n' "$name"
            printf '======================================================================\n'
            perf report -i "$data" --stdio --no-children 2>/dev/null || true
            printf '\n'
        } | tee -a "$report"

        # Flamegraph SVG (requires FlameGraph scripts)
        if [[ -d "$FLAMEGRAPH_DIR" ]]; then
            local svg="$PERF_DIR/${name}.svg"
            if perf script -i "$data" 2>/dev/null \
                    | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
                    | "$FLAMEGRAPH_DIR/flamegraph.pl" \
                    > "$svg"; then
                log "  flamegraph: $svg"
            else
                log "  WARNING: flamegraph generation failed for $name"
                rm -f "$svg"
            fi
        fi
    done

    log "Combined text report : $report"
    log "Per-process SVGs     : $PERF_DIR/*.svg"
}

on_interrupt() {
    log "Interrupted — shutting down cleanly ..."
    shutdown_apps
    wait_for_perf
    generate_reports
    exit 130
}
trap on_interrupt INT TERM

# ------------------------------------------------------------------------------
# Preflight checks
[[ -x "$FIX8_BIN" ]]   || die "f8test not found or not executable: $FIX8_BIN"
command -v perf >/dev/null 2>&1 || die "'perf' not found in PATH"

mkdir -p "$LOG_DIR" "$PERF_DIR"

log "=== perf_run ==="
log "  install prefix : $PREFIX"
log "  perf output    : $PERF_DIR"
log "  call-graph     : $CALLGRAPH  (freq=${FREQ}Hz)"

# Launch apps in dependency order (mirrors start_fix_seq_system.py)
launch_app "arbiter_primary"        "arbiter"                "$ETC_DIR/arbiter/arbiter.toml"
sleep "$STARTUP_DELAY"
launch_app "arbiter_secondary"      "arbiter"                "$ETC_DIR/arbiter/arbiter_secondary.toml"
sleep "$STARTUP_DELAY"
launch_app "sample_fix_gateway_seq" "sample_fix_gateway_seq" "$ETC_DIR/sample_fix_gateway_seq/sample_fix_gateway_seq.toml"
sleep "$STARTUP_DELAY"
launch_app "sequencer_primary"      "sequencer"              "$ETC_DIR/sequencer/sequencer.toml"
sleep "$STARTUP_DELAY"
launch_app "sequencer_secondary"    "sequencer"              "$ETC_DIR/sequencer/sequencer_secondary.toml"
sleep "$STARTUP_DELAY"
launch_app "matching_engine"        "matching_engine"        "$ETC_DIR/matching_engine/matching_engine.toml"

log "Settling for ${SETTLE_TIME}s ..."
sleep "$SETTLE_TIME"

# Verify all apps are still running before attaching perf
for i in "${!APP_NAMES[@]}"; do
    if ! kill -0 "${APP_PIDS[$i]}" 2>/dev/null; then
        die "${APP_NAMES[$i]} (PID ${APP_PIDS[$i]}) died during startup"
    fi
done

log "=== Attaching perf to all processes ==="
for i in "${!APP_NAMES[@]}"; do
    attach_perf "${APP_NAMES[$i]}" "${APP_PIDS[$i]}"
done
sleep 1   # ensure perf is recording before orders arrive

log "=== Sending 1000 NOS via fix8 ==="
(cd "$FIX8_DIR" && printf 'T\n' | "$FIX8_BIN" -c "$FIX8_CFG" -N GW1)
log "fix8 session complete — waiting ${POST_FIX8_WAIT}s for pipeline to drain ..."

sleep "$POST_FIX8_WAIT"

shutdown_apps
wait_for_perf
generate_reports

log "=== Done ==="
