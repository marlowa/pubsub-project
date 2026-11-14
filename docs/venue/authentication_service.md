# Authentication Service {#authentication_service}

## Role

The authentication service (`applications/authentication_service/`) validates member logons on
behalf of both order gateways, FIX and binary. When a member logs on, the gateway does not check
the password itself — it runs a SCRAM exchange against the authentication service and admits the
session only on a `Granted` result.

It is a small C++ reactor application with one application thread. It holds one piece of state: an
in-memory map of `comp_id → SCRAM credential`, loaded at startup and mutated at runtime by the
admin service (see the **Credential Management** section below).

It runs as an **active/active pair** for HA — instances `a` and `b`, both serving the gateway
at once. It is *not* an arbiter-elected leader/follower pair; see the **HA and Failover**
section below.

## PDU Protocol

Defined in `applications/authentication.dsl`, namespace `pubsub_itc_fw_app`. Two families:

**FIX-logon authentication (gateway ↔ auth, network port):**

| ID | Message | Key fields |
|----|---------|-----------|
| 500 | `AuthenticationRequest` | `request_id` (i64), `comp_id` (string), `client_nonce` (bytes) |
| 501 | `AuthenticationChallenge` | `request_id`, `server_nonce`, `salt`, `iterations` (i32) |
| 502 | `AuthenticationProof` | `request_id`, `client_proof` (32 bytes) |
| 503 | `AuthenticationResult` | `request_id`, `outcome` (enum), `server_signature` (32 bytes), `force_password_change` (bool); on a grant, the comp id's provisioning: cancel-on-disconnect, gateway instances, and the three throttle limits `max_place_per_second`, `max_amend_per_second`, `max_cancel_per_second` |
| 504 | `AuthenticationHeartbeat` | `heartbeat_number` (i64); sent by the gateway on each connection at its heartbeat interval |
| 505 | `AuthenticationHeartbeatAnswer` | `heartbeat_number`, echoed; sent by the service in reply to each heartbeat |

`request_id` carries the gateway's `ConnectionID` for the FIX session unchanged through all
four messages, so the gateway correlates the result with the correct pending session.

Messages 504 and 505 keep the connection open and let the gateway see that the service is answering;
see **Keeping the connection open** below.

**Credential management (admin service ↔ auth, TLS admin port):**

| ID | Message | Purpose |
|----|---------|---------|
| 510 / 511 | `SetCredentialRequest` / `Result` | Add or replace a compID's credential (from a plaintext password) |
| 512 / 513 | `RemoveCredentialRequest` / `Result` | Remove a compID's credential |
| 514 / 515 | `RestoreCredentialRequest` / `Result` | Install a pre-derived SCRAM credential (stored/server keys, salt, iterations) |

## SCRAM Authentication Flow

```
FIX client sends Logon
    │
    ▼
Order Gateway  ──AuthenticationRequest(request_id=conn_id, comp_id, client_nonce)──▶  Auth Service
                                                                                        │ look up comp_id
    ◀──────────────AuthenticationChallenge(server_nonce, salt, iterations)──────────────┘
    │ derive client_proof
    ├──────────────AuthenticationProof(client_proof)───────────────────────────────────▶ verify
    ◀──────────────AuthenticationResult(outcome, server_signature)──────────────────────┘
    │
    ▼
Gateway admits (Granted) or rejects (else) the FIX session; verifies server_signature
```

Each exchange is self-contained: SCRAM's nonces are per-exchange and ephemeral, and the
service never persists anything about an in-flight logon. The only durable input is the
credential map.

## Which classes do the work

`ScramExchanges` decides what to answer to each `AuthenticationRequest` and `AuthenticationProof`,
and holds the exchanges between a challenge and its proof, keyed by the gateway's `request_id`.
`AuthenticationThread` decodes each message, asks `ScramExchanges`, and sends and logs the answer.
The split lets the decisions be tested without a reactor: `authentication_service_tests` runs a
real exchange against `ScramExchanges`, deriving each proof from a password as a gateway does, and
checks the configuration loader.

An unknown comp id is answered with `UnknownUser` at once, with no challenge. A wrong password is
answered with `BadPassword`. Both are logged at Info, because each is something a member did and the
service handled.

## Keeping the connection open {#authentication_heartbeat}

Each order gateway holds one connection to each authentication service instance for as long as it
runs. Between logons it has nothing to send on it, and the service closes any connection that has
sent it nothing for `[network] connection_inactivity_limit` (600 seconds in every environment). So
each gateway sends an `AuthenticationHeartbeat` on each connection every
`[timeouts] authentication_heartbeat_interval` (30 seconds), and the service answers each one with an
`AuthenticationHeartbeatAnswer`. A connection carrying heartbeats is never silent for long enough to
be closed. The service's limit still matters: it closes the connection of a gateway that has gone
without closing its socket.

The answers let the gateway tell whether the service is still answering. A service whose process
hangs, or whose machine disappears without closing its sockets, leaves the connection open, so the
gateway would otherwise go on sending logons to it, and each would wait out the 10-second
`scram_auth_timeout` before being refused. Instead the gateway treats anything arriving on the
connection, an answer or part of an exchange, as proof that the service is answering. If nothing has
arrived for three heartbeat intervals, it closes the connection, logs a Warning, and asks the reactor
to connect again (`ApplicationThread::close_and_reconnect`). Logons go to the other instance
meanwhile. Three intervals rather than one, so that a service slow to answer during a burst of logons
does not have a working connection closed.

A new connection is not given logons until something has arrived on it. The gateway sends a
heartbeat as soon as the connection is made, so this takes one round trip. The wait matters because
the operating system completes a connection to a process that is stopped but still holds its
listening socket: a connection being made says nothing about whether the service will answer.

`fix_common::AuthenticationServiceLink` holds this for one connection, and both gateways use it.

Heartbeats and their answers are logged at Debug. A connection becoming usable is logged at Info, a
connection closed because the service stopped answering at Warning, and a connection lost for any
other reason at Warning.

## Credential Management

The authentication service is **not** the writer of the credential state — the admin service
is. The admin service updates the database (the durable source of truth) and pushes the change
to the auth service over the TLS **admin port** as a `SetCredential` / `RemoveCredential` /
`RestoreCredential` PDU, which mutates the in-memory map.

Because HA is active/active, the admin service fans each change out to **both** auth instances
so neither drifts stale (see [admin service](admin_service.md) and
[WAL and HA → Authentication Service HA](../availability/wal_and_ha.md#ha_auth_service)).
On startup, each instance loads the full credential set from `credentials.toml` (a database
export produced by `db/export_credentials.py`), so a restarted instance is current as of that
export.

The export also carries each comp id's provisioning: cancel-on-disconnect, gateway instances and
throttle limits. The service loads them with the credential, sends them on `AuthenticationResult`
when a logon is granted, and writes them back out whenever it rewrites `credentials.toml` after a
credential change; a field it did not write back would be lost at the next restart. A throttle
limit absent from the file is read as 0, no limit, and one outside 0 to 100,000 stops the service
from starting, naming the credential.

## HA and Failover

Active/active, caller-selected — see
[WAL and HA → Authentication Service HA](../availability/wal_and_ha.md#ha_auth_service)
for the full model. In short:

- Both instances (`a`, `b`) run and serve simultaneously; neither is elected, neither is
  promoted, and there is no arbiter, WAL, or epoch/fencing involved.
- The gateway connects to both and has a try-first/backup preference (a *caller* preference,
  not an election). It sends a logon to instance `a` when `a` is answering, and to `b` when `a`
  is not. If no instance is connected and answering, the logon is refused: the FIX gateway sends
  a Logout with the text "Authentication service unavailable", and the binary gateway refuses it
  with `AuthenticationUnavailable`. An already-established session is unaffected (it does not
  re-authenticate); only new logons exercise the failover.
- An instance counts as not answering when its connection is lost, and also when nothing has
  arrived on the connection for three heartbeat intervals; see **Keeping the connection open**.
- Verified by `ha_test.py` scenario 17 (kill instance `a`; a fresh FIX logon is authenticated
  by instance `b`) and scenario 110 (stop instance `a` with SIGSTOP and leave it stopped; the
  gateway closes its connection to `a`, and a fresh logon is authenticated by `b`). Scenario 110
  also checks that neither connection is lost while both instances answer, with the gateway's
  heartbeat at 1 second and the services' inactivity limit at 4.

## Port Allocation

Two listeners per instance, with the same ports in every environment file:

| Listener | Instance `a` | Instance `b` | Peer |
|----------|:------------:|:------------:|------|
| `[network]` — logon authentication (plain TCP) | 11070 | 11071 | Order gateways |
| `[admin]` — credential management (TLS) | 11072 | 11073 | Admin service |

The admin channel is TLS (the service presents `admin.crt`; client-certificate verification is
off by default). The network channel carries the SCRAM exchange only, and is not encrypted.

## Configuration

`authentication_service_a.toml` / `authentication_service_b.toml`:

| Section / key | Purpose |
|---------------|---------|
| `credentials_file` | Path to the credential export loaded at startup (`credentials.toml`) |
| `[network] listen_host / listen_port` | Gateway-facing SCRAM listener |
| `[network] connection_inactivity_limit` | How long a gateway's connection may send nothing before the service closes it; 600 seconds. Must be well above the gateways' `authentication_heartbeat_interval` |
| `[admin] listen_port` | Admin-service credential-management listener |
| `[admin] tls_certificate_path / tls_private_key_path / tls_ca_path` | Admin-channel TLS; `tls_require_client_certificate` gates mTLS |
| `[logging]`, `[metrics]`, `[reactor]`, `[event_queue_pool]`, `[command_queue_pool]` | Standard framework sections |

Config placeholders use the `auth_service_a_*` / `auth_service_b_*` namespace, expanded from
the `[auth_service_a]` / `[auth_service_b]` sections of the environment TOML by `deploy.py`.

## See Also

- [Secure Comms](../operations/secure_comms.md) — SCRAM design, TLS, and the auth PDU protocol
- [WAL and High Availability](../availability/wal_and_ha.md#ha_auth_service) — the active/active HA model
- [Admin Service](admin_service.md) — the single writer of credential state
- [Order Gateway](fix_order_gateway.md) — the caller that runs the SCRAM exchange per logon
