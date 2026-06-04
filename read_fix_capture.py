#!/usr/bin/env python3
"""
read_fix_capture.py — decode and display a FIX binary capture file.

Usage:
    ./read_fix_capture.py <capture_file> [options]

Options:
    --count-only     Print record count and summary statistics only.
    --inbound-only   Print only inbound (FIX client → gateway) records.
    --outbound-only  Print only outbound (gateway → FIX client) records.
    --max N          Stop after printing N records.
    --raw            Print raw hex bytes instead of ASCII FIX text.

Record format (little-endian):
    uint32_t  payload_size    -- byte count of the FIX message
    int64_t   timestamp_ns    -- nanoseconds since Unix epoch (wall clock)
    uint8_t   direction       -- 0 = inbound, 1 = outbound
    uint8_t   data[...]       -- raw FIX wire bytes

Exit code: 0 on success, 1 on error.
"""

import argparse
import datetime
import struct
import sys
from pathlib import Path

_HEADER_FORMAT = "<IqB"   # uint32 + int64 + uint8 = 13 bytes
_HEADER_SIZE   = struct.calcsize(_HEADER_FORMAT)

_INBOUND  = 0
_OUTBOUND = 1


def _format_timestamp(timestamp_ns: int) -> str:
    """Convert nanoseconds-since-epoch to a human-readable UTC string."""
    if timestamp_ns <= 0:
        return f"<invalid:{timestamp_ns}>"
    dt = datetime.datetime.fromtimestamp(timestamp_ns / 1e9, tz=datetime.timezone.utc)
    frac_us = (timestamp_ns % 1_000_000_000) // 1000
    return dt.strftime("%Y-%m-%dT%H:%M:%S") + f".{frac_us:06d}Z"


def _format_fix(raw: bytes) -> str:
    """Replace SOH (0x01) with '|' for readable display."""
    return raw.replace(b"\x01", b"|").decode("ascii", errors="replace")


def _extract_fix_tag(raw: bytes, tag: int) -> str:
    """Return the value of a FIX tag from the raw wire bytes, or '' if absent."""
    prefix = f"\x01{tag}=".encode("ascii")
    idx = raw.find(prefix)
    if idx < 0:
        # Tag might be at the very start (tag 8 = BeginString)
        prefix2 = f"{tag}=".encode("ascii")
        if raw.startswith(prefix2):
            idx = -len(prefix)  # trick: will be handled below as start
            end = raw.find(b"\x01")
            return raw[len(prefix2):end].decode("ascii", errors="replace") if end > 0 else ""
        return ""
    start = idx + len(prefix)
    end = raw.find(b"\x01", start)
    return raw[start:end].decode("ascii", errors="replace") if end > 0 else ""


def read_records(path: Path):
    """Generator: yield (payload_size, timestamp_ns, direction, data) for each record."""
    with open(path, "rb") as fh:
        while True:
            header_bytes = fh.read(_HEADER_SIZE)
            if len(header_bytes) < _HEADER_SIZE:
                break
            payload_size, timestamp_ns, direction = struct.unpack(_HEADER_FORMAT, header_bytes)
            data = fh.read(payload_size) if payload_size > 0 else b""
            if len(data) < payload_size:
                print(f"WARNING: truncated record at end of file (expected {payload_size} bytes, got {len(data)})",
                      file=sys.stderr)
                break
            yield payload_size, timestamp_ns, direction, data


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("capture_file", type=Path, help="Path to the binary capture file")
    parser.add_argument("--count-only", action="store_true",
                        help="Print record count and summary only")
    parser.add_argument("--inbound-only", action="store_true",
                        help="Print only inbound records")
    parser.add_argument("--outbound-only", action="store_true",
                        help="Print only outbound records")
    parser.add_argument("--max", type=int, default=0, metavar="N",
                        help="Stop after N records (0 = no limit)")
    parser.add_argument("--raw", action="store_true",
                        help="Print raw hex bytes instead of ASCII FIX text")
    args = parser.parse_args()

    if not args.capture_file.is_file():
        print(f"error: file not found: {args.capture_file}", file=sys.stderr)
        return 1

    total = 0
    inbound_count = 0
    outbound_count = 0
    printed = 0

    for payload_size, timestamp_ns, direction, data in read_records(args.capture_file):
        total += 1
        if direction == _INBOUND:
            inbound_count += 1
            direction_str = "IN "
        elif direction == _OUTBOUND:
            outbound_count += 1
            direction_str = "OUT"
        else:
            direction_str = f"??{direction}"

        if args.inbound_only and direction != _INBOUND:
            continue
        if args.outbound_only and direction != _OUTBOUND:
            continue
        if args.max > 0 and printed >= args.max:
            continue

        if not args.count_only:
            ts_str = _format_timestamp(timestamp_ns)
            msg_type = _extract_fix_tag(data, 35)
            msg_type_str = f" [{msg_type}]" if msg_type else ""
            if args.raw:
                hex_str = data.hex()
                print(f"{ts_str}  {direction_str}  {payload_size:5d}b{msg_type_str}  {hex_str}")
            else:
                fix_str = _format_fix(data)
                print(f"{ts_str}  {direction_str}  {payload_size:5d}b{msg_type_str}  {fix_str}")
        printed += 1

    print(f"\n--- {total} records total: {inbound_count} inbound, {outbound_count} outbound ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
