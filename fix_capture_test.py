#!/usr/bin/env python3
"""
fix_capture_test.py — Integration test for FIX message capture.

Starts auth_service_primary and order_gateway with fix_capture.enabled=true,
sends a small FIX session (Logon, 3 x NewOrderSingle, Logout) using a minimal
built-in FIX client, shuts down, then reads and validates the capture file.

Because no sequencer is running, the gateway rejects every NewOrderSingle
immediately with a gateway-synthesised reject ExecutionReport.  This is enough
to verify that all three capture points fire correctly:
  - inbound: Logon, NOS, Logout captured
  - outbound: Logon reply, reject ERs, Logout reply captured

The test uses a plain Python FIX client so it has no dependency on f8test or
any external FIX engine config file.

Run from the project root:
    ./fix_capture_test.py [install_prefix]
"""

import argparse
import datetime
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ── tunables ──────────────────────────────────────────────────────────────────
STARTUP_DELAY       = 0.2   # seconds between process launch and log polling
LOG_POLL_INTERVAL   = 0.05  # seconds between log-file polls
SHUTDOWN_TIMEOUT    = 5.0   # seconds for SIGTERM grace period before SIGKILL
AUTH_READY_TIMEOUT  = 5.0   # seconds for auth service to start
GW_READY_TIMEOUT    = 10.0  # seconds to wait for gateway TCP port to open
FIX_REPLY_TIMEOUT   = 10.0  # seconds to wait for a FIX reply
# ──────────────────────────────────────────────────────────────────────────────

# Comp IDs used in the FIX session.
# CLIENT_ONE must be present in the auth service stub credentials (see below).
_FIX_SENDER = "CLIENT_ONE"
_FIX_TARGET = "GATEWAY"

# Stub SCRAM credential for all test comp_ids (derived from "stubpassword").
_STUB_STORED_KEY = "e0eaf13bf630627621a7f47e378fb8c62c5b4bb709d42767d0193dc537f34be2"
_STUB_SERVER_KEY = "c016b7864891fe5bad757b60de234df09dde5a4be4deb015e158ca1aae9bec7d"
_STUB_SALT       = "0102030405060708090a0b0c0d0e0f10"
_STUB_ITERATIONS = 4096
_STUB_PASSWORD   = "stubpassword"

_TEST_COMP_IDS = [
    "TEST_GATEWAY", "GATEWAY_ALPHA", "GATEWAY_BETA",
    "CLIENT_ONE", "CLIENT_TWO", "CLIENT_THREE",
]

_AUTH_READY_MARKER = "AuthenticationService: PDU listener on"

_SOH = "\x01"


# ── FIX message building ──────────────────────────────────────────────────────

def _fix_timestamp() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S")


def _fix_checksum(msg: str) -> str:
    return f"{sum(ord(c) for c in msg) % 256:03d}"


def _build_fix(msg_type: str, seq_num: int, extra_fields: list) -> bytes:
    """Construct a complete FIX 4.2 message with correct BodyLength and Checksum."""
    body_fields = [
        ("35", msg_type),
        ("34", str(seq_num)),
        ("49", _FIX_SENDER),
        ("52", _fix_timestamp()),
        ("56", _FIX_TARGET),
    ] + extra_fields
    body = "".join(f"{t}={v}{_SOH}" for t, v in body_fields)
    header = f"8=FIXT.1.1{_SOH}9={len(body)}{_SOH}"
    pre_trailer = header + body
    trailer = f"10={_fix_checksum(pre_trailer)}{_SOH}"
    return (pre_trailer + trailer).encode("ascii")


def _build_logon(seq_num: int) -> bytes:
    return _build_fix("A", seq_num, [("98", "0"), ("108", "30")])


def _build_nos(seq_num: int, cl_ord_id: str) -> bytes:
    ts = _fix_timestamp()
    return _build_fix("D", seq_num, [
        ("11", cl_ord_id),
        ("21", "1"),
        ("55", "BHP"),
        ("54", "1"),
        ("60", ts),
        ("40", "2"),
        ("44", "100.00"),
        ("38", "100"),
    ])


def _build_logout(seq_num: int) -> bytes:
    return _build_fix("5", seq_num, [])


# ── FIX message receiving ─────────────────────────────────────────────────────

_FIX_END_RE = re.compile(rb"\x0110=\d{3}\x01")


def _recv_fix(sock: socket.socket, timeout: float) -> bytes:
    """Receive one complete FIX message from the socket."""
    sock.settimeout(timeout)
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            raise EOFError("connection closed while waiting for FIX message")
        buf += chunk
        if _FIX_END_RE.search(buf):
            return buf


def _fix_msg_type(raw: bytes) -> str:
    """Extract MsgType (tag 35) from a raw FIX message."""
    m = re.search(rb"\x0135=([^\x01]+)", raw)
    return m.group(1).decode("ascii") if m else ""


# ── utilities ─────────────────────────────────────────────────────────────────

class TestFailure(Exception):
    pass


def log(msg: str) -> None:
    ts = datetime.datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def die(msg: str) -> None:
    log(f"FAIL: {msg}")
    raise TestFailure(msg)


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _poll_log_for(log_path: Path, marker: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as fh:
                if marker in fh.read():
                    return True
        time.sleep(LOG_POLL_INTERVAL)
    return False


def _wait_for_tcp_port(host: str, port: int, timeout: float) -> bool:
    """Poll until a TCP connection to host:port succeeds."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except (socket.error, OSError):
            time.sleep(0.1)
    return False


def _launch(name: str, binary: Path, log_path: Path, config_path: Path) -> subprocess.Popen:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    stdout_path = log_path.parent / f"{name}.stdout"
    with open(stdout_path, "w") as stdout_fh:
        proc = subprocess.Popen(
            [str(binary), str(log_path), str(config_path)],
            cwd=str(log_path.parent),
            stdout=stdout_fh,
            stderr=subprocess.STDOUT,
        )
    proc.stdout_path = stdout_path  # type: ignore[attr-defined]
    log(f"  {name} — PID {proc.pid}")
    return proc


def _show_stdout(proc: subprocess.Popen) -> None:
    """Print the captured stdout/stderr of a failed process for diagnosis."""
    stdout_path = getattr(proc, "stdout_path", None)
    if stdout_path is None or not Path(stdout_path).is_file():
        return
    content = Path(stdout_path).read_text(errors="replace").strip()
    if content:
        log(f"  --- process output ---")
        for line in content.splitlines():
            log(f"  {line}")
        log(f"  --- end output ---")


def _shutdown(name: str, proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=SHUTDOWN_TIMEOUT)
    except subprocess.TimeoutExpired:
        log(f"  WARNING: {name} did not exit within {SHUTDOWN_TIMEOUT:.0f}s — SIGKILL")
        proc.kill()
        proc.wait()


# ── config writers ────────────────────────────────────────────────────────────

def _generate_tls_cert(directory: Path) -> tuple:
    """Generate a self-signed RSA cert/key pair. Returns (cert_path, key_path)."""
    cert_path = directory / "test_admin.crt"
    key_path  = directory / "test_admin.key"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048",
         "-keyout", str(key_path), "-out", str(cert_path),
         "-days", "1", "-nodes", "-subj", "/CN=localhost/O=capture-test"],
        check=True, capture_output=True,
    )
    return cert_path, key_path


def _write_auth_service_toml(path: Path, listen_port: int, admin_port: int,
                              cert_path: Path, key_path: Path) -> None:
    credentials_path = path.parent / "credentials.toml"
    cred_blocks = []
    for comp_id in _TEST_COMP_IDS:
        cred_blocks.append(
            f'[[credential]]\n'
            f'comp_id    = "{comp_id}"\n'
            f'stored_key = "{_STUB_STORED_KEY}"\n'
            f'server_key = "{_STUB_SERVER_KEY}"\n'
            f'salt       = "{_STUB_SALT}"\n'
            f'iterations = {_STUB_ITERATIONS}\n'
        )
    credentials_path.write_text("\n".join(cred_blocks))
    path.write_text(
        f'credentials_file = "{credentials_path}"\n'
        f'\n'
        f'[network]\n'
        f'listen_host = "127.0.0.1"\n'
        f'listen_port = {listen_port}\n'
        f'\n'
        f'[admin]\n'
        f'listen_port                    = {admin_port}\n'
        f'tls_certificate_path           = "{cert_path}"\n'
        f'tls_private_key_path           = "{key_path}"\n'
        f'tls_ca_path                    = ""\n'
        f'tls_require_client_certificate = false\n'
        f'\n'
        f'[logging]\n'
        f'applog_level     = "debug"\n'
        f'syslog_level     = "critical"\n'
        f'mode             = "none"\n'
        f'max_file_size    = 10240000\n'
        f'max_backup_files = 10\n'
        f'\n'
        f'[reactor]\n'
        f'cpu_pinning_enabled      = false\n'
        f'cpu_pinning_reserve_cpu0 = true\n'
        f'cpu_registry_lock_file   = "/dev/shm/pubsub_cpu_registry_captest.lock"\n'
        f'\n'
        f'[event_queue_pool]\n'
        f'objects_per_slab = 64\n'
        f'initial_slabs    = 1\n'
        f'\n'
        f'[command_queue_pool]\n'
        f'objects_per_slab = 64\n'
        f'initial_slabs    = 1\n'
    )


def _write_gateway_toml(path: Path, fix_port: int, er_port: int,
                         auth_port: int, capture_file: Path) -> None:
    # Use a dummy sequencer port — the gateway will retry the connection in the
    # background but that is harmless; no orders are forwarded in this test.
    dummy_seq_port = _find_free_port()
    path.write_text(
        f'[network]\n'
        f'listen_host         = "127.0.0.1"\n'
        f'listen_port         = {fix_port}\n'
        f'raw_buffer_capacity = 65536\n'
        f'er_listen_host      = "127.0.0.1"\n'
        f'er_listen_port      = {er_port}\n'
        f'\n'
        f'[authentication_service]\n'
        f'host           = "127.0.0.1"\n'
        f'port           = {auth_port}\n'
        f'secondary_host = "127.0.0.1"\n'
        f'secondary_port = {auth_port}\n'
        f'scram_password = "{_STUB_PASSWORD}"\n'
        f'\n'
        f'[sequencer]\n'
        f'ha_enabled   = false\n'
        f'primary_host = "127.0.0.1"\n'
        f'primary_port = {dummy_seq_port}\n'
        f'\n'
        f'[fix_session]\n'
        f'sender_comp_id         = "GATEWAY"\n'
        f'default_target_comp_id = "CLIENT"\n'
        f'\n'
        f'[timeouts]\n'
        f'logon_timeout      = "30s"\n'
        f'scram_auth_timeout = "10s"\n'
        f'\n'
        f'[logging]\n'
        f'applog_level     = "debug"\n'
        f'syslog_level     = "critical"\n'
        f'mode             = "none"\n'
        f'max_file_size    = 10240000\n'
        f'max_backup_files = 10\n'
        f'\n'
        f'[reactor]\n'
        f'cpu_pinning_enabled    = false\n'
        f'cpu_pinning_reserve_cpu0  = true\n'
        f'cpu_registry_lock_file = "/dev/shm/pubsub_cpu_registry_captest.lock"\n'
        f'\n'
        f'[event_queue_pool]\n'
        f'objects_per_slab = 64\n'
        f'initial_slabs    = 1\n'
        f'\n'
        f'[command_queue_pool]\n'
        f'objects_per_slab = 64\n'
        f'initial_slabs    = 1\n'
        f'\n'
        f'[fix_capture]\n'
        f'enabled     = true\n'
        f'file        = "{capture_file}"\n'
        f'queue_depth = 1000\n'
    )


# ── capture file reader ───────────────────────────────────────────────────────

_CAPTURE_HEADER_FORMAT = "<IqB"
_CAPTURE_HEADER_SIZE   = struct.calcsize(_CAPTURE_HEADER_FORMAT)


def _read_capture_records(path: Path) -> list:
    records = []
    with open(path, "rb") as fh:
        while True:
            header = fh.read(_CAPTURE_HEADER_SIZE)
            if len(header) < _CAPTURE_HEADER_SIZE:
                break
            payload_size, timestamp_ns, direction = struct.unpack(_CAPTURE_HEADER_FORMAT, header)
            data = fh.read(payload_size) if payload_size > 0 else b""
            if len(data) < payload_size:
                break
            records.append((payload_size, timestamp_ns, direction, data))
    return records


# ── test runner ───────────────────────────────────────────────────────────────

def run_test(prefix: Path) -> bool:
    bin_dir = prefix / "bin"
    auth_binary = bin_dir / "authentication_service"
    gw_binary   = bin_dir / "order_gateway"

    for binary in (auth_binary, gw_binary):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            die(f"binary not found or not executable: {binary}")

    result_pass = False
    auth_proc   = None
    gw_proc     = None

    try:
        with tempfile.TemporaryDirectory(prefix="fix_capture_test_") as tmp_str:
            tmp = Path(tmp_str)
            log(f"  temp directory: {tmp}")

            auth_port  = _find_free_port()
            admin_port = _find_free_port()
            fix_port   = _find_free_port()
            er_port    = _find_free_port()

            capture_file = tmp / "fix_capture.bin"
            auth_log     = tmp / "authentication_service.log"
            gw_log       = tmp / "order_gateway.log"
            auth_toml    = tmp / "authentication_service.toml"
            gw_toml      = tmp / "order_gateway.toml"

            cert_path, key_path = _generate_tls_cert(tmp)
            _write_auth_service_toml(auth_toml, auth_port, admin_port, cert_path, key_path)
            _write_gateway_toml(gw_toml, fix_port, er_port, auth_port, capture_file)

            # ── start processes ────────────────────────────────────────────────
            log("=== Starting authentication_service ===")
            auth_proc = _launch("authentication_service", auth_binary, auth_log, auth_toml)
            time.sleep(STARTUP_DELAY)
            if auth_proc.poll() is not None:
                _show_stdout(auth_proc)
                die(f"authentication_service exited immediately (code {auth_proc.returncode})")
            if not _poll_log_for(auth_log, _AUTH_READY_MARKER, AUTH_READY_TIMEOUT):
                die(f"authentication_service did not become ready within {AUTH_READY_TIMEOUT:.0f}s")
            log("  authentication_service: ready")

            log("=== Starting order_gateway ===")
            gw_proc = _launch("order_gateway", gw_binary, gw_log, gw_toml)
            time.sleep(STARTUP_DELAY)
            if gw_proc.poll() is not None:
                _show_stdout(gw_proc)
                die(f"order_gateway exited immediately (code {gw_proc.returncode})")
            if not _wait_for_tcp_port("127.0.0.1", fix_port, GW_READY_TIMEOUT):
                _show_stdout(gw_proc)
                die(f"order_gateway FIX port {fix_port} did not open within {GW_READY_TIMEOUT:.0f}s")
            log(f"  order_gateway: FIX port {fix_port} accepting connections")

            # Wait for the gateway's outbound PDU connection to the auth service
            # to be established.  If we send a FIX Logon before this connection
            # is up, the gateway has no auth service to forward the SCRAM exchange
            # to and immediately rejects the session with a FIX Logout.
            auth_conn_marker = "authentication service connection"
            if not _poll_log_for(gw_log, auth_conn_marker, GW_READY_TIMEOUT):
                die(f"order_gateway did not establish auth service connection within {GW_READY_TIMEOUT:.0f}s")
            log("  order_gateway: auth service connection established")

            # ── FIX session ────────────────────────────────────────────────────
            log("=== Running FIX session ===")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("127.0.0.1", fix_port))
            seq = 1

            log("  Sending Logon (35=A) ...")
            sock.sendall(_build_logon(seq))
            seq += 1

            log(f"  Waiting for Logon reply (timeout {FIX_REPLY_TIMEOUT:.0f}s) ...")
            reply = _recv_fix(sock, FIX_REPLY_TIMEOUT)
            reply_type = _fix_msg_type(reply)
            if reply_type not in ("A", "5"):
                die(f"expected Logon reply (35=A) or Logout (35=5), got MsgType={reply_type!r}")
            if reply_type == "5":
                die("gateway rejected the session (sent Logout instead of Logon reply)")
            log("  Logon reply received: OK")

            log("  Sending 3 x NewOrderSingle (35=D) ...")
            for i in range(3):
                sock.sendall(_build_nos(seq, f"ORD-{i+1}"))
                seq += 1

            log("  Waiting for 3 x ExecutionReport (reject) ...")
            for i in range(3):
                er = _recv_fix(sock, FIX_REPLY_TIMEOUT)
                er_type = _fix_msg_type(er)
                if er_type != "8":
                    die(f"expected ExecutionReport (35=8), got MsgType={er_type!r} for order {i+1}")
            log("  All 3 reject ERs received: OK")

            log("  Sending Logout (35=5) ...")
            sock.sendall(_build_logout(seq))
            reply = _recv_fix(sock, FIX_REPLY_TIMEOUT)
            logout_type = _fix_msg_type(reply)
            if logout_type != "5":
                die(f"expected Logout reply (35=5), got MsgType={logout_type!r}")
            log("  Logout reply received: OK")
            sock.close()

            # ── shutdown ───────────────────────────────────────────────────────
            log("=== Shutting down processes ===")
            _shutdown("order_gateway",          gw_proc)
            _shutdown("authentication_service", auth_proc)
            gw_proc   = None
            auth_proc = None

            # ── validate capture file ──────────────────────────────────────────
            log("=== Validating capture file ===")
            if not capture_file.is_file():
                die("capture file does not exist after shutdown")
            file_size = capture_file.stat().st_size
            if file_size == 0:
                die("capture file is empty")
            log(f"  capture file: {file_size} bytes")

            records = _read_capture_records(capture_file)
            if not records:
                die("capture file parsed as zero records")
            log(f"  total records: {len(records)}")

            inbound_records  = [(sz, ts, d, data) for sz, ts, d, data in records if d == 0]
            outbound_records = [(sz, ts, d, data) for sz, ts, d, data in records if d == 1]
            log(f"  inbound: {len(inbound_records)}  outbound: {len(outbound_records)}")

            # Verify binary format: all direction bytes must be 0 or 1.
            bad_direction = [d for _, _, d, _ in records if d not in (0, 1)]
            if bad_direction:
                die(f"records with invalid direction byte: {bad_direction}")
            log("  all direction bytes valid (0 or 1): OK")

            # Timestamps must be positive.
            bad_ts = [ts for _, ts, _, _ in records if ts <= 0]
            if bad_ts:
                die(f"{len(bad_ts)} records have non-positive timestamp")
            log("  all timestamps positive: OK")

            # Must have at least one inbound Logon.
            has_inbound_logon = any(b"35=A" in data for _, _, _, data in inbound_records)
            if not has_inbound_logon:
                die("no inbound Logon (35=A) found in capture file")
            log("  inbound Logon (35=A): found")

            # Must have at least one outbound Logon reply.
            has_outbound_logon = any(b"35=A" in data for _, _, _, data in outbound_records)
            if not has_outbound_logon:
                die("no outbound Logon reply (35=A) found in capture file")
            log("  outbound Logon reply (35=A): found")

            # Must have at least one inbound NOS.
            has_inbound_nos = any(b"35=D" in data for _, _, _, data in inbound_records)
            if not has_inbound_nos:
                die("no inbound NewOrderSingle (35=D) found in capture file")
            log("  inbound NewOrderSingle (35=D): found")

            # Must have at least one outbound ExecutionReport (reject).
            has_outbound_er = any(b"35=8" in data for _, _, _, data in outbound_records)
            if not has_outbound_er:
                die("no outbound ExecutionReport (35=8) found in capture file")
            log("  outbound ExecutionReport (35=8): found")

            # Must have at least one inbound Logout.
            has_inbound_logout = any(b"35=5" in data for _, _, _, data in inbound_records)
            if not has_inbound_logout:
                die("no inbound Logout (35=5) found in capture file")
            log("  inbound Logout (35=5): found")

            result_pass = True

    except TestFailure:
        pass
    except KeyboardInterrupt:
        log("Interrupted")
    finally:
        if gw_proc is not None:
            _shutdown("order_gateway", gw_proc)
        if auth_proc is not None:
            _shutdown("authentication_service", auth_proc)

    return result_pass


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "prefix", nargs="?", default="installed",
        metavar="install_prefix",
        help="Path to the cmake install prefix (default: installed)",
    )
    args = parser.parse_args()

    script_dir  = Path(__file__).resolve().parent
    prefix_path = Path(args.prefix)
    if not prefix_path.is_absolute():
        prefix_path = (script_dir / prefix_path).resolve()

    lib_dir  = str(prefix_path / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    log("=" * 60)
    log("  fix_capture_test")
    log("=" * 60)
    log(f"  install prefix: {prefix_path}")
    log("")

    passed = run_test(prefix_path)

    log("")
    log("=" * 60)
    log(f"  RESULT: {'PASS' if passed else 'FAIL'}")
    log("=" * 60)

    if not passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
