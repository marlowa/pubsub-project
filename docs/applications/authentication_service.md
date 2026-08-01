# Authentication Service {#authentication_service}

## Role

The authentication service (`applications/authentication_service/`) validates FIX client
logons on behalf of the FIX order gateway. When a FIX client sends a Logon, the gateway does not
check the password itself — it runs a SCRAM exchange against the authentication service and
admits the session only on a `Granted` result.

It is a small, single-threaded C++ reactor application. It holds one piece of state: an
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
| 503 | `AuthenticationResult` | `request_id`, `outcome` (enum), `server_signature` (32 bytes), `force_password_change` (bool) |

`request_id` carries the gateway's `ConnectionID` for the FIX session unchanged through all
four messages, so the gateway correlates the result with the correct pending session.

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

## Credential Management

The authentication service is **not** the writer of the credential state — the admin service
is. The admin service updates the database (the durable source of truth) and pushes the change
to the auth service over the TLS **admin port** as a `SetCredential` / `RemoveCredential` /
`RestoreCredential` PDU, which mutates the in-memory map.

Because HA is active/active, the admin service fans each change out to **both** auth instances
so neither drifts stale (see [admin service](admin_service.md) and
[WAL and HA → Authentication Service HA](../design/wal_and_ha.md#authentication-service-ha)).
On startup, each instance loads the full credential set from `credentials.toml` (a database
export produced by `db/export_credentials.py`), so a restarted instance is current as of that
export.

## HA and Failover

Active/active, caller-selected — see
[WAL and HA → Authentication Service HA](../design/wal_and_ha.md#authentication-service-ha)
for the full model. In short:

- Both instances (`a`, `b`) run and serve simultaneously; neither is elected, neither is
  promoted, and there is no arbiter, WAL, or epoch/fencing involved.
- The gateway connects to both and has a try-first/backup preference (a *caller* preference,
  not an election). If the preferred instance dies, the gateway authenticates the next logon
  against the survivor. An already-established FIX session is unaffected (it does not
  re-authenticate); only new logons exercise the failover.
- Verified by `ha_test.py` scenario 17 (kill instance `a`; a fresh FIX logon is authenticated
  by instance `b`).

## Port Allocation

Two listeners per instance (example ports; see the environment TOML for actual values):

| Listener | Instance `a` | Instance `b` | Peer |
|----------|:------------:|:------------:|------|
| `[network]` — FIX-logon authentication | 7070 | 7071 | Order gateway |
| `[admin]` — credential management (TLS) | 7072 | 7073 | Admin service |

The admin channel is TLS (the service presents `admin.crt`; client-certificate verification is
off by default). The network channel carries the SCRAM exchange only.

## Configuration

`authentication_service_a.toml` / `authentication_service_b.toml`:

| Section / key | Purpose |
|---------------|---------|
| `credentials_file` | Path to the credential export loaded at startup (`credentials.toml`) |
| `[network] listen_host / listen_port` | Gateway-facing SCRAM listener |
| `[admin] listen_port` | Admin-service credential-management listener |
| `[admin] tls_certificate_path / tls_private_key_path / tls_ca_path` | Admin-channel TLS; `tls_require_client_certificate` gates mTLS |
| `[logging]`, `[reactor]`, `[event_queue_pool]`, `[command_queue_pool]` | Standard framework sections |

Config placeholders use the `auth_service_a_*` / `auth_service_b_*` namespace, expanded from
the `[auth_service_a]` / `[auth_service_b]` sections of the environment TOML by `deploy.py`.

## See Also

- [Secure Comms](../design/secure_comms.md) — SCRAM design, TLS, and the auth PDU protocol
- [WAL and High Availability](../design/wal_and_ha.md#authentication-service-ha) — the active/active HA model
- [Admin Service](admin_service.md) — the single writer of credential state
- [Order Gateway](fix_order_gateway.md) — the caller that runs the SCRAM exchange per logon
