#!/usr/bin/env python3
"""
auth_service_test.py — Integration tests for the authentication service.

Starts the authentication_service binary with a temporary TLS configuration,
connects as a PDU client over TLS, executes SCRAM-SHA-256 exchange scenarios,
and verifies the outcomes.  Reports PASS/FAIL and observed details.

Run from the project root:
    ./auth_service_test.py [install_prefix] [options]

Scenarios
---------
  1  single_exchange
       One TLS connection. One full AuthenticationRequest → AuthenticationChallenge
       → AuthenticationProof → AuthenticationResult(Granted) exchange.
       Verifies: request_id is echoed correctly; server_nonce starts with
       client_nonce; salt is non-empty; iterations is positive; outcome is
       Granted; force_password_change is False; service log confirms.

  2  sequential_exchanges_same_connection
       One TLS connection. Two sequential exchanges with distinct request_ids
       and distinct comp_ids. Verifies: each exchange gets its own correctly-
       correlated response; the second exchange succeeds after the first has
       completed; both exchanges appear in the service log.

  3  multiple_clients
       Three independent TLS connections (sequential) each performing one full
       exchange. Verifies: the service handles multiple connection/teardown
       cycles; all three exchanges complete with Granted; all appear in the
       service log.

Options:
    install_prefix      Path to cmake install prefix (default: build/installed)
    --scenario N|all    Scenario to run, or 'all' (default: all)
    --ready-timeout S   Max seconds for service startup (default: 5)
    --reply-timeout S   Max seconds to wait for each PDU reply (default: 5)
"""

import argparse
import hashlib
import hmac as hmac_module
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

# ── tunables ──────────────────────────────────────────────────────────────────
STARTUP_DELAY     = 0.2    # seconds after launch before polling the log
LOG_POLL_INTERVAL = 0.05   # seconds between log-file polls
SHUTDOWN_TIMEOUT  = 5.0    # seconds for SIGTERM grace period before SIGKILL
LOG_CONFIRM_TIMEOUT = 2.0  # seconds to wait for a log line after a PDU exchange

# ── PDU framing ───────────────────────────────────────────────────────────────
# PduHeader (24 bytes, all fields big-endian):
#   byte_count(I4)  pdu_id(H2)  version(B1)  filler_a(B1)
#   seq_no(q8)      canary(I4)  filler_b(I4)
PDU_HEADER_FORMAT = "!IHBBqII"
PDU_HEADER_SIZE   = struct.calcsize(PDU_HEADER_FORMAT)   # == 24
PDU_CANARY        = 0xC0FFEE00
PDU_VERSION       = 1

PDU_ID_AUTHENTICATION_REQUEST   = 500
PDU_ID_AUTHENTICATION_CHALLENGE = 501
PDU_ID_AUTHENTICATION_PROOF     = 502
PDU_ID_AUTHENTICATION_RESULT    = 503

AUTHENTICATION_OUTCOME_GRANTED = 0

# Stub password shared between the service and this test client.
# Must match stub_password in AuthenticationThread.cpp.
STUB_PASSWORD = "stubpassword"

# Substrings expected in the service log to confirm key events.
_SERVICE_READY_MARKER   = "AuthenticationService: TLS listener on"
_SERVICE_GRANTED_MARKER = "AuthenticationThread: AuthenticationResult Granted"


# ── SCRAM-SHA-256 client-side computation ─────────────────────────────────────

def _compute_auth_message(comp_id: str, client_nonce: bytes, server_nonce: bytes,
                           salt: bytes, iterations: int) -> bytes:
    """Build the canonical AuthMessage matching compute_auth_message() in ScramCrypto.cpp."""
    def encode_blob(data: bytes) -> bytes:
        return struct.pack("<I", len(data)) + data
    return (encode_blob(comp_id.encode("utf-8")) +
            encode_blob(client_nonce) +
            encode_blob(server_nonce) +
            encode_blob(salt) +
            struct.pack("<I", iterations))


def _scram_compute(password: str, salt: bytes, iterations: int,
                   comp_id: str, client_nonce: bytes,
                   server_nonce: bytes) -> tuple[bytes, bytes]:
    """
    Compute (client_proof, expected_server_signature) for a SCRAM-SHA-256 exchange.

    Returns a tuple of:
      client_proof            -- 32 bytes to send in AuthenticationProof
      expected_server_signature -- 32 bytes to compare against the AuthenticationResult
    """
    salted_password = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)

    client_key = hmac_module.new(salted_password, b"Client Key", hashlib.sha256).digest()
    stored_key = hashlib.sha256(client_key).digest()
    server_key = hmac_module.new(salted_password, b"Server Key", hashlib.sha256).digest()

    auth_message = _compute_auth_message(comp_id, client_nonce, server_nonce, salt, iterations)

    client_signature = hmac_module.new(stored_key, auth_message, hashlib.sha256).digest()
    client_proof = bytes(a ^ b for a, b in zip(client_key, client_signature))

    expected_server_signature = hmac_module.new(server_key, auth_message, hashlib.sha256).digest()

    return client_proof, expected_server_signature


# ── utilities ─────────────────────────────────────────────────────────────────

class TestFailure(Exception):
    """Raised by die() to abort the current scenario with RESULT: FAIL."""


def log(message: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {message}", flush=True)


def die(message: str) -> None:
    log(f"FAIL: {message}")
    raise TestFailure(message)


# ── PDU payload codec (all payload fields are little-endian) ──────────────────

def _encode_i64(value: int) -> bytes:
    return struct.pack("<q", value)

def _encode_i32(value: int) -> bytes:
    return struct.pack("<i", value)

def _encode_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded

def _encode_bytes_field(value: bytes) -> bytes:
    return struct.pack("<I", len(value)) + value


def _decode_i64(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<q", data, offset)[0], offset + 8

def _decode_i32(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<i", data, offset)[0], offset + 4

def _decode_bytes_field(data: bytes, offset: int) -> tuple[bytes, int]:
    length = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    return data[offset : offset + length], offset + length

def _decode_string(data: bytes, offset: int) -> tuple[str, int]:
    raw, new_offset = _decode_bytes_field(data, offset)
    return raw.decode("utf-8"), new_offset

def _decode_bool(data: bytes, offset: int) -> tuple[bool, int]:
    return bool(data[offset]), offset + 1


def _encode_authentication_request(request_id: int, comp_id: str, client_nonce: bytes) -> bytes:
    return _encode_i64(request_id) + _encode_string(comp_id) + _encode_bytes_field(client_nonce)

def _encode_authentication_proof(request_id: int, client_proof: bytes) -> bytes:
    return _encode_i64(request_id) + _encode_bytes_field(client_proof)


@dataclass
class AuthenticationChallenge:
    request_id:   int
    server_nonce: bytes
    salt:         bytes
    iterations:   int

@dataclass
class AuthenticationResult:
    request_id:            int
    outcome:               int
    server_signature:      bytes
    force_password_change: bool


def _decode_authentication_challenge(payload: bytes) -> AuthenticationChallenge:
    offset = 0
    request_id,   offset = _decode_i64(payload, offset)
    server_nonce, offset = _decode_bytes_field(payload, offset)
    salt,         offset = _decode_bytes_field(payload, offset)
    iterations,   offset = _decode_i32(payload, offset)
    return AuthenticationChallenge(request_id, server_nonce, salt, iterations)

def _decode_authentication_result(payload: bytes) -> AuthenticationResult:
    offset = 0
    request_id,            offset = _decode_i64(payload, offset)
    outcome,               offset = _decode_i32(payload, offset)
    server_signature,      offset = _decode_bytes_field(payload, offset)
    force_password_change, offset = _decode_bool(payload, offset)
    return AuthenticationResult(request_id, outcome, server_signature, force_password_change)


# ── PDU send / receive ─────────────────────────────────────────────────────────

def _recv_exact(connection: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed before all expected bytes arrived")
        data += chunk
    return data


def _send_pdu(connection: socket.socket, pdu_id: int, payload: bytes) -> None:
    header = struct.pack(PDU_HEADER_FORMAT,
                         len(payload), pdu_id, PDU_VERSION, 0, 0, PDU_CANARY, 0)
    connection.sendall(header + payload)


def _recv_pdu(connection: socket.socket, timeout: float) -> tuple[int, bytes]:
    """Receive one complete PDU. Returns (pdu_id, payload)."""
    connection.settimeout(timeout)
    header_bytes = _recv_exact(connection, PDU_HEADER_SIZE)
    byte_count, pdu_id, _version, _filler_a, _seq_no, canary, _filler_b = \
        struct.unpack(PDU_HEADER_FORMAT, header_bytes)
    if canary != PDU_CANARY:
        raise ValueError(f"PDU canary mismatch: expected 0x{PDU_CANARY:08X}, got 0x{canary:08X}")
    payload = _recv_exact(connection, byte_count) if byte_count > 0 else b""
    return pdu_id, payload


# ── TCP client ─────────────────────────────────────────────────────────────────

def _connect_tcp(host: str, port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    return sock


# ── service helpers ────────────────────────────────────────────────────────────

def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _write_test_toml(path: Path, listen_port: int) -> None:
    path.write_text(
        f'[network]\n'
        f'listen_host = "127.0.0.1"\n'
        f'listen_port = {listen_port}\n'
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
        f'cpu_pinning_dev_mode   = true\n'
        f'cpu_registry_lock_file = "/dev/shm/pubsub_cpu_registry.lock"\n'
        f'\n'
        f'[event_queue_pool]\n'
        f'objects_per_slab = 64\n'
        f'initial_slabs    = 1\n'
        f'\n'
        f'[command_queue_pool]\n'
        f'objects_per_slab = 64\n'
        f'initial_slabs    = 1\n'
    )


def _launch_service(binary_path: Path, log_path: Path,
                    config_path: Path) -> subprocess.Popen:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    stdout_path = log_path.parent / "authentication_service.stdout"
    with open(stdout_path, "w") as stdout_file:
        process = subprocess.Popen(
            [str(binary_path), str(log_path), str(config_path)],
            stdout=stdout_file,
            stderr=subprocess.STDOUT,
        )
    log(f"  authentication_service — PID {process.pid}")
    return process


def _wait_for_service_ready(log_path: Path, timeout: float) -> float:
    """
    Poll the service log for the TLS listener ready marker.
    Returns elapsed seconds. Raises TestFailure on timeout.
    """
    deadline = time.monotonic() + timeout
    t0 = time.monotonic()
    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as log_file:
                if _SERVICE_READY_MARKER in log_file.read():
                    return time.monotonic() - t0
        time.sleep(LOG_POLL_INTERVAL)
    die(f"authentication_service did not become ready within {timeout:.0f}s")


def _poll_log_for(log_path: Path, marker: str,
                  timeout: float, from_byte: int = 0) -> tuple[bool, float]:
    """
    Poll log_path for a line containing marker, starting at from_byte.
    Returns (found, elapsed_seconds).
    """
    deadline = time.monotonic() + timeout
    position = from_byte
    t0 = time.monotonic()
    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as log_file:
                log_file.seek(position)
                chunk = log_file.read()
                position = log_file.tell()
            if marker in chunk:
                return True, time.monotonic() - t0
        time.sleep(LOG_POLL_INTERVAL)
    return False, time.monotonic() - t0


def _shutdown_service(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=SHUTDOWN_TIMEOUT)
    except subprocess.TimeoutExpired:
        log(f"  WARNING: service did not exit within {SHUTDOWN_TIMEOUT:.0f}s — SIGKILL")
        process.kill()
        process.wait()


# ── SCRAM exchange helper ──────────────────────────────────────────────────────

def _do_scram_exchange(
    connection: socket.socket,
    request_id: int,
    comp_id: str,
    client_nonce: bytes,
    reply_timeout: float,
) -> tuple[AuthenticationChallenge, AuthenticationResult]:
    """
    Perform one full SCRAM-SHA-256 exchange on an established connection:
      Send AuthenticationRequest -> Receive AuthenticationChallenge ->
      Send AuthenticationProof  -> Receive AuthenticationResult.

    Computes the real ClientProof from STUB_PASSWORD and verifies the
    ServerSignature returned by the service.  Returns (challenge, result).
    Raises TestFailure if any check fails.
    """
    # Step 1 -- send AuthenticationRequest.
    _send_pdu(connection, PDU_ID_AUTHENTICATION_REQUEST,
              _encode_authentication_request(request_id, comp_id, client_nonce))

    # Step 2 -- receive AuthenticationChallenge.
    received_pdu_id, payload = _recv_pdu(connection, reply_timeout)
    if received_pdu_id != PDU_ID_AUTHENTICATION_CHALLENGE:
        die(f"expected PDU {PDU_ID_AUTHENTICATION_CHALLENGE} (AuthenticationChallenge), "
            f"got {received_pdu_id}")
    challenge = _decode_authentication_challenge(payload)

    if challenge.request_id != request_id:
        die(f"challenge request_id mismatch: expected {request_id}, "
            f"got {challenge.request_id}")
    if not challenge.server_nonce.startswith(client_nonce):
        die("server_nonce does not begin with client_nonce")
    if len(challenge.salt) == 0:
        die("challenge salt is empty")
    if challenge.iterations <= 0:
        die(f"challenge iterations must be positive, got {challenge.iterations}")

    # Step 3 -- compute real ClientProof and send AuthenticationProof.
    client_proof, expected_server_sig = _scram_compute(
        STUB_PASSWORD, challenge.salt, challenge.iterations,
        comp_id, client_nonce, challenge.server_nonce)

    _send_pdu(connection, PDU_ID_AUTHENTICATION_PROOF,
              _encode_authentication_proof(request_id, client_proof))

    # Step 4 -- receive AuthenticationResult.
    received_pdu_id, payload = _recv_pdu(connection, reply_timeout)
    if received_pdu_id != PDU_ID_AUTHENTICATION_RESULT:
        die(f"expected PDU {PDU_ID_AUTHENTICATION_RESULT} (AuthenticationResult), "
            f"got {received_pdu_id}")
    result = _decode_authentication_result(payload)

    if result.request_id != request_id:
        die(f"result request_id mismatch: expected {request_id}, "
            f"got {result.request_id}")

    # Verify ServerSignature -- defeats MITM impersonation of the auth service.
    if result.outcome == AUTHENTICATION_OUTCOME_GRANTED:
        if result.server_signature != expected_server_sig:
            die(f"ServerSignature mismatch for request_id={request_id} comp_id={comp_id} "
                f"-- possible MITM or server crypto error")

    return challenge, result


# ── scenario runners ───────────────────────────────────────────────────────────

def _run_scenario_1(service_log: Path, port: int, args) -> bool:
    """Single full SCRAM exchange on one connection."""
    request_id   = 1
    comp_id      = "TEST_GATEWAY"
    client_nonce = os.urandom(16)
    log_pos      = service_log.stat().st_size if service_log.is_file() else 0

    log("=== Connecting client ===")
    try:
        connection = _connect_tcp("127.0.0.1", port)
    except Exception as exc:
        die(f"connect failed: {exc}")
    log("  connection established")

    log("=== Executing SCRAM exchange ===")
    try:
        challenge, result = _do_scram_exchange(
            connection, request_id, comp_id, client_nonce,
            args.reply_timeout)
        connection.close()
    except TestFailure:
        raise
    except Exception as exc:
        die(f"exchange failed: {exc}")

    log("=== Verifying outcome ===")
    if result.outcome != AUTHENTICATION_OUTCOME_GRANTED:
        die(f"expected outcome Granted (0), got {result.outcome}")
    if result.force_password_change:
        die("expected force_password_change=False, got True")

    log(f"  request_id echo       : OK ({request_id})")
    log(f"  server_nonce prefix   : OK (starts with client_nonce)")
    log(f"  salt length           : {len(challenge.salt)} bytes")
    log(f"  iterations            : {challenge.iterations}")
    log(f"  outcome               : Granted ({result.outcome})")
    log(f"  server_signature      : OK ({len(result.server_signature)} bytes verified)")
    log(f"  force_password_change : {result.force_password_change}")

    granted_marker = (f"{_SERVICE_GRANTED_MARKER} "
                      f"request_id={request_id} comp_id={comp_id}")
    found, elapsed = _poll_log_for(service_log, granted_marker,
                                   LOG_CONFIRM_TIMEOUT, log_pos)
    if not found:
        die(f"service log did not contain '{granted_marker}' within "
            f"{LOG_CONFIRM_TIMEOUT:.0f}s")
    log(f"  service log confirmed : OK ({elapsed:.2f}s)")

    return True


def _run_scenario_2(service_log: Path, port: int, args) -> bool:
    """Two sequential exchanges on a single connection."""
    log_pos = service_log.stat().st_size if service_log.is_file() else 0

    log("=== Connecting client ===")
    try:
        connection = _connect_tcp("127.0.0.1", port)
    except Exception as exc:
        die(f"connect failed: {exc}")
    log("  connection established")

    exchanges = [
        (1, "GATEWAY_ALPHA", os.urandom(16)),
        (2, "GATEWAY_BETA",  os.urandom(16)),
    ]

    for request_id, comp_id, client_nonce in exchanges:
        log(f"=== Exchange request_id={request_id} comp_id={comp_id} ===")
        try:
            _, result = _do_scram_exchange(
                connection, request_id, comp_id, client_nonce,
                args.reply_timeout)
        except TestFailure:
            raise
        except Exception as exc:
            die(f"exchange request_id={request_id}: {exc}")

        if result.outcome != AUTHENTICATION_OUTCOME_GRANTED:
            die(f"exchange request_id={request_id}: expected Granted (0), "
                f"got {result.outcome}")
        log(f"  outcome: Granted — OK")

    connection.close()

    log("=== Verifying service log ===")
    for request_id, comp_id, _ in exchanges:
        marker = (f"{_SERVICE_GRANTED_MARKER} "
                  f"request_id={request_id} comp_id={comp_id}")
        found, _ = _poll_log_for(service_log, marker,
                                  LOG_CONFIRM_TIMEOUT, log_pos)
        if not found:
            die(f"service log missing: '{marker}'")
    log("  service log confirmed: OK (both exchanges logged)")

    return True


def _run_scenario_3(service_log: Path, port: int, args) -> bool:
    """Three independent connections each with one full exchange."""
    log_pos = service_log.stat().st_size if service_log.is_file() else 0

    clients = [
        (10, "CLIENT_ONE",   os.urandom(16)),
        (11, "CLIENT_TWO",   os.urandom(16)),
        (12, "CLIENT_THREE", os.urandom(16)),
    ]

    log("=== Running three independent client exchanges ===")
    for request_id, comp_id, client_nonce in clients:
        log(f"  comp_id={comp_id} request_id={request_id}")
        try:
            connection = _connect_tcp("127.0.0.1", port)
            _, result = _do_scram_exchange(
                connection, request_id, comp_id, client_nonce,
                args.reply_timeout)
            connection.close()
        except TestFailure:
            raise
        except Exception as exc:
            die(f"client {comp_id}: exchange failed: {exc}")

        if result.outcome != AUTHENTICATION_OUTCOME_GRANTED:
            die(f"client {comp_id}: expected Granted (0), got {result.outcome}")
        log(f"    outcome: Granted — OK")

    log("=== Verifying service log ===")
    for request_id, comp_id, _ in clients:
        marker = (f"{_SERVICE_GRANTED_MARKER} "
                  f"request_id={request_id} comp_id={comp_id}")
        found, _ = _poll_log_for(service_log, marker,
                                  LOG_CONFIRM_TIMEOUT, log_pos)
        if not found:
            die(f"service log missing: '{marker}'")
    log("  service log confirmed: OK (all three clients logged)")

    return True


# ── scenario registry ──────────────────────────────────────────────────────────

_SCENARIOS = [
    (1, "single_exchange",
     "Single full SCRAM exchange (one connection, one request_id)",
     "outcome=Granted; request_id echoed; server_nonce starts with client_nonce",
     _run_scenario_1),
    (2, "sequential_exchanges_same_connection",
     "Two sequential exchanges on one TLS connection",
     "both exchanges complete with Granted; each request_id correctly correlated",
     _run_scenario_2),
    (3, "multiple_clients",
     "Three independent TLS connections each performing one full exchange",
     "all three exchanges complete with Granted; all logged in service log",
     _run_scenario_3),
]

_SCENARIO_MAP = {
    number: (short_name, description, expected_outcome, runner)
    for number, short_name, description, expected_outcome, runner in _SCENARIOS
}


# ── per-scenario driver ────────────────────────────────────────────────────────

def run_scenario(
    number: int,
    binary_path: Path,
    temp_dir: Path,
    args,
) -> bool:
    short_name, description, expected_outcome, runner = _SCENARIO_MAP[number]

    listen_port = _find_free_port()
    config_path = temp_dir / f"scenario_{number}.toml"
    log_path    = temp_dir / f"scenario_{number}" / "authentication_service.log"

    _write_test_toml(config_path, listen_port)

    log("=" * 60)
    log(f"  auth_service_test  —  Scenario {number}: {description}")
    log("=" * 60)
    log(f"  port             : {listen_port}")
    log(f"  expected outcome : {expected_outcome}")
    log("")

    process     = None
    result_pass = False

    try:
        log("=== Starting authentication_service ===")
        process = _launch_service(binary_path, log_path, config_path)

        time.sleep(STARTUP_DELAY)
        if process.poll() is not None:
            die(f"authentication_service exited immediately "
                f"(exit code {process.returncode})")

        log(f"  Waiting for service ready "
            f"(timeout {args.ready_timeout:.0f}s) ...")
        elapsed = _wait_for_service_ready(log_path, args.ready_timeout)
        log(f"  service ready in {elapsed:.2f}s")
        log("")

        result_pass = runner(log_path, listen_port, args)

    except TestFailure:
        pass
    except KeyboardInterrupt:
        log("Interrupted — shutting down ...")
    finally:
        if process is not None:
            log("")
            log("=== Shutting down authentication_service ===")
            _shutdown_service(process)
            log("  done")

    log("")
    log("=" * 60)
    log(f"  RESULT  : {'PASS' if result_pass else 'FAIL'}")
    log(f"  scenario: {number} — {description}")
    log("=" * 60)

    return result_pass


# ── main ──────────────────────────────────────────────────────────────────────

def _scenario_type(value: str):
    if value == "all":
        return "all"
    try:
        return int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"'{value}' is not a valid scenario number or 'all'"
        )


def main() -> None:
    valid_scenario_numbers = [entry[0] for entry in _SCENARIOS]

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--scenario", type=_scenario_type, default="all", metavar="N|all",
        help=(
            "Scenario to run, or 'all' to run every scenario in order. "
            "Valid numbers: "
            + ", ".join(str(n) for n in valid_scenario_numbers)
            + ".  (default: all)"
        ),
    )
    parser.add_argument(
        "prefix", nargs="?", default="build/installed",
        metavar="install_prefix",
        help="Path to the cmake install prefix (default: build/installed)",
    )
    parser.add_argument(
        "--ready-timeout", type=float, default=5.0, metavar="SECS",
        help="Max seconds for service startup (default: 5)",
    )
    parser.add_argument(
        "--reply-timeout", type=float, default=5.0, metavar="SECS",
        help="Max seconds to wait for each PDU reply (default: 5)",
    )
    args = parser.parse_args()

    script_dir  = Path(__file__).resolve().parent
    raw_prefix  = args.prefix
    prefix_path = Path(raw_prefix)
    if not prefix_path.is_absolute():
        prefix_path = (script_dir / prefix_path).resolve()

    if not prefix_path.is_dir():
        print(f"error: install prefix '{prefix_path}' does not exist",
              file=sys.stderr)
        sys.exit(1)

    binary_path = prefix_path / "bin" / "authentication_service"
    if not binary_path.is_file() or not os.access(binary_path, os.X_OK):
        print(f"error: binary not found or not executable: {binary_path}",
              file=sys.stderr)
        sys.exit(1)

    lib_dir  = str(prefix_path / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    if args.scenario == "all":
        scenarios_to_run = valid_scenario_numbers
    else:
        scenario_number = args.scenario
        if scenario_number not in _SCENARIO_MAP:
            print(f"error: unknown scenario {scenario_number}; valid: "
                  + ", ".join(str(n) for n in valid_scenario_numbers),
                  file=sys.stderr)
            sys.exit(1)
        scenarios_to_run = [scenario_number]

    with tempfile.TemporaryDirectory(prefix="auth_service_test_") as tmp_str:
        temp_dir = Path(tmp_str)
        log(f"Temporary directory: {temp_dir}")
        log("")

        overall_pass = True
        results: list[tuple[int, str, bool]] = []

        for scenario_number in scenarios_to_run:
            short_name = _SCENARIO_MAP[scenario_number][0]
            passed = run_scenario(
                scenario_number, binary_path,
                temp_dir, args,
            )
            results.append((scenario_number, short_name, passed))
            if not passed:
                overall_pass = False
            log("")

        if len(results) > 1:
            passed_count = sum(1 for _, _, p in results if p)
            log("=" * 60)
            log(f"SUMMARY: {passed_count}/{len(results)} scenarios passed")
            for num, name, passed in results:
                log(f"  Scenario {num} ({name}): {'PASS' if passed else 'FAIL'}")
            log("=" * 60)

    if not overall_pass:
        sys.exit(1)


if __name__ == "__main__":
    main()
