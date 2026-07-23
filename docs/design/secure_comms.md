# Secure Communications

## Overview

The framework provides two complementary security mechanisms:

- **TLS** — transport-layer confidentiality and integrity for byte-stream connections
- **SCRAM-SHA-256** — mutual password-based authentication for FIX gateway clients

These are independent layers. TLS protects the channel; SCRAM proves the client knows the
correct password. In the full production design the FIX gateway listener uses `TlsRawBytes`
so that SCRAM exchanges and FIX messages are both protected in transit. Until TLS is wired
to the gateway listener they are complementary but not combined.

---

## TLS

### Status

Implemented with OpenSSL and in use. The order gateway registers an encrypted FIX listener
(`TlsRawBytes`) *alongside* its plain listener, gated by the `[fix_tls]` config (`enabled`,
`cert`, `key`, and `tls_listen_port`); it has been live-verified with a QuickFIX client
speaking TLS. The authentication service listener is likewise TLS-secured
(`tls_certificate_path` / `tls_private_key_path`, an optional CA path, and
`tls_require_client_certificate` for mutual TLS), so the gateway-to-auth link is encrypted.
The plain (non-TLS) listeners remain available for local and test deployments.

### Design Principle: Memory BIOs

The reactor thread must never block on I/O. TLS adds the complication that `SSL_read` and
`SSL_write` cannot be called directly on a non-blocking socket without careful layering,
because OpenSSL may need to write handshake records at any point during a read (and vice
versa).

The solution is **OpenSSL memory BIOs**: the socket is read and written using `recv` /
`send` with `MSG_DONTWAIT`, and OpenSSL operates against in-memory `BIO` objects that hold
the ciphertext. The reactor feeds received bytes into the read BIO, then calls `SSL_read`
to drain plaintext out. For writes, `SSL_write` puts ciphertext into the write BIO, which
the reactor then flushes to the socket. Neither `SSL_read` nor `SSL_write` ever touches the
socket fd directly, so they never block.

### TLS Version and Cipher Cap

All contexts enforce **TLS 1.2 only** (`SSL_CTX_set_min_proto_version(TLS1_2_VERSION)`,
`SSL_CTX_set_max_proto_version(TLS1_2_VERSION)`).

The cap exists because QuickFIX/J's MINA `SslFilter` does not handle TLS 1.3
`NewSessionTicket` records correctly: it deadlocks waiting for a response that it never
sends, causing the FIX client (`fix-test-client`) to time out on logon. Until the FIX test
client is upgraded to a TLS-1.3-capable version, TLS 1.3 is disabled at the framework
level.

The cap is applied in `TlsContext::apply_common_tls_options()` for both server and client
contexts. When the cap is lifted, removing those two `set_*_proto_version` calls and adding
TLS 1.3 cipher groups (`TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256`) will be
sufficient.

Cipher suites (TLS 1.2): AEAD only — `ECDHE-RSA-AES256-GCM-SHA384` and
`ECDHE-ECDSA-AES256-GCM-SHA384`. `SSL_OP_NO_COMPRESSION` is also set.

### Key Classes

**`TlsContext`** (`TlsContext.hpp` / `.cpp`)

Wraps an `SSL_CTX*`. Non-copyable. Two factory methods:

| Method | Side | Notes |
|--------|------|-------|
| `create_server(cert, key, ca, require_client_cert)` | Server | `ca` empty → client certificate not verified |
| `create_client(ca, cert, key)` | Client | `ca` empty → server certificate not verified |

One `TlsContext` per listener or per outbound service. Certificate and key loading happens
once at construction; each accepted connection creates one `SSL*` from the shared `SSL_CTX`.

**`TlsState`** (`TlsState.hpp` / `.cpp`)

Per-connection state. Owns:
- `SSL*` — the per-connection OpenSSL object
- `BIO* rbio` — read BIO; reactor writes received ciphertext here
- `BIO* wbio` — write BIO; OpenSSL writes ciphertext here for the reactor to flush
- `pending_outbound` — `std::vector<uint8_t>` of ciphertext bytes that could not be sent
  immediately (TCP send buffer full)
- `HandshakePhase` enum — `Pending`, `Complete`, `Failed`

`TlsState` is move-constructible because `OutboundConnection` is move-inserted into the
connections map.

**`TlsRawBytesProtocolHandler`** (`TlsRawBytesProtocolHandler.hpp` / `.cpp`)

Implements `ProtocolHandlerInterface`. Selected by `ProtocolType::TlsRawBytes`.

| Operation | Behaviour |
|-----------|-----------|
| Construction | `is_server` flag selects `SSL_accept` vs `SSL_connect` path |
| `start_outbound_handshake()` | Generates `ClientHello` and flushes the write BIO to the socket. Called once by `OutboundConnectionManager` immediately after TCP connect completes. |
| Subsequent handshake steps | Driven by `on_data_ready()` arrivals from epoll; `drive_handshake()` called internally until `HandshakePhase::Complete` |
| `ConnectionEstablished` delivery | **Not delivered until handshake is complete** |
| `on_data_ready()` post-handshake | `recv()` into read BIO → `drain_plaintext()` loops `SSL_read()` into `MirroredBuffer` → enqueues `RawSocketCommunication` event |
| `send_prebuilt()` | Calls `SSL_write()` (copies plaintext internally), releases slab chunk **immediately** (before network send completes), flushes write BIO to socket; unsent ciphertext goes into `pending_outbound` |
| `continue_send()` | Drains `pending_outbound` on `EPOLLOUT` |
| Peer close | `SSL_ERROR_ZERO_RETURN` (TLS `close_notify`) → `{false, ""}` → `ConnectionLost` |
| Backpressure | Same high-water (75%) / low-water (50%) mark scheme as `RawBytesProtocolHandler` |

Note on slab lifetime: for TLS, the slab chunk is freed inside `send_prebuilt()` rather
than on send completion, because `SSL_write` copies the plaintext into OpenSSL's internal
buffer. The slab is no longer needed after `SSL_write` returns.

**`TlsListenerConfiguration`** (`TlsListenerConfiguration.hpp`)

Fields: `certificate_path`, `private_key_path`, `ca_path`, `require_client_certificate`.
Carried as `std::optional<TlsListenerConfiguration>` inside `InboundListenerConfiguration`.
When present, the reactor calls `TlsContext::create_server` during init and stores the
context in the `InboundListener`. The `InboundConnectionManager` creates a
`TlsRawBytesProtocolHandler` for each accepted connection.

**`TlsClientConfiguration`** (`TlsClientConfiguration.hpp`)

Fields: `ca_path`, `certificate_path`, `private_key_path`, `raw_buffer_capacity`. Carried
as `std::optional<TlsClientConfiguration>` inside `ServiceEndpoints`. When present,
`OutboundConnectionManager` creates a `TlsContext` and a `TlsRawBytesProtocolHandler`
for the connection instead of a `PduProtocolHandler`.

### ProtocolHandlerInterface Additions for TLS

Three virtual methods were added to `ProtocolHandlerInterface` to support TLS:

| Method | TLS handler | Non-TLS default |
|--------|-------------|-----------------|
| `start_outbound_handshake()` | Sends `ClientHello` | `{true, ""}` (no-op) |
| `is_handshake_complete()` | Returns true once `HandshakePhase::Complete` | `true` always |
| `is_reads_paused()` | True when backpressure is active | `false` always |

### Known Bug: EPOLLOUT After Handshake Flush (Fixed 2026-06-15)

**Symptom:** FIX logon stuck — the gateway logged "authentication succeeded" and "FIX OUT"
but the client never received the Logon response.

**Root cause:** `InboundConnectionManager::on_data_ready()` called `flush_wbio()` during
the TLS handshake. If the flush returned `EAGAIN` (TCP send buffer full), `has_pending_send()`
became true. The `pause_reads` branch checked `has_pending_send()` and armed `EPOLLOUT`
correctly, but the non-pause path had no such check — `EPOLLOUT` was never registered.
Any subsequent `SendRaw` stashed itself in `pending_send_` and was never retried because
`EPOLLOUT` never fired.

**Fix:** Added `else if (conn.handler()->has_pending_send())` to `on_data_ready()` that
registers `EPOLLIN | EPOLLOUT | EPOLLERR` whenever there is pending outbound ciphertext,
regardless of backpressure state.

### Integration Tests

All certificates are generated programmatically in tests via the OpenSSL C API
(EC prime256v1, SHA-256); no external tooling is required.

| Test | Scenario |
|------|----------|
| `TlsHandshakeAndRoundTrip` | Inbound: client establishes TLS, sends framed message, receives reply |
| `FragmentedCiphertextDelivery` | Inbound: length prefix in first `SSL_write`, payload 20 ms later; framework accumulates both records |
| `PeerDisconnect` | Inbound: `SSL_shutdown` → `close_notify` → `ConnectionLost` |
| `MutualTlsHandshake` | Inbound: server requires client certificate; both sides authenticate |
| `HandshakeFailure` | Inbound: client has wrong CA; TLS alert → server tears down → `ConnectionLost` |
| `OutboundTlsHandshakeAndRoundTrip` | Outbound: reactor as TLS client; send on `ConnectionEstablished`; server replies; `ConnectionLost` on server close |
| `OutboundMutualTls` | Outbound: server requires client certificate; `TlsClientConfiguration` carries cert/key paths |
| `OutboundTlsServerDisconnect` | Outbound: server closes after handshake; `ConnectionEstablished` delivered before `ConnectionLost` |
| `OutboundTlsHandshakeFailureNoConnectionEstablished` | Outbound: wrong trust anchor; cert verification fails; `ConnectionEstablished` never delivered; reactor stays alive |

---

## SCRAM-SHA-256 Authentication

### Why SCRAM

SCRAM (Salted Challenge Response Authentication Mechanism, RFC 5802) provides mutual
authentication without transmitting the password in plaintext or storing it in recoverable
form. The server stores only derived key material (`StoredKey`, `ServerKey`). A database
breach does not expose plaintext passwords and cannot be used to impersonate the server.
The gateway verifies the `ServerSignature` in the `AuthenticationResult`, confirming the
service is genuine.

### Architecture

A standalone application (`applications/authentication_service/`) handles all
authentication. Two instances run **active/active** for HA — `a` (port 7070) and `b`
(port 7071); both serve the gateway, and the gateway falls over to the other instance if one
dies. Each SCRAM *exchange* is stateless and self-contained within four PDU messages — but
the credential set the instances validate against **is** mutable shared state. The admin
service is its single writer: it updates the database and fans every credential change out to
both instances (`SetCredential`/`RemoveCredential` PDUs) so their in-memory copies stay in
sync. So the instances *do* require synchronisation — via the admin fan-out — even though no
individual auth exchange carries state. See
[WAL and High Availability → Authentication Service HA](wal_and_ha.md#authentication-service-ha)
for the full model.

### PDU Protocol

Defined in `applications/authentication.dsl`, namespace `pubsub_itc_fw_app`.

| ID | Message | Key fields |
|----|---------|-----------|
| 500 | `AuthenticationRequest` | `request_id` (i64), `comp_id` (string), `client_nonce` (bytes) |
| 501 | `AuthenticationChallenge` | `request_id`, `server_nonce` (bytes), `salt` (bytes), `iterations` (i32) |
| 502 | `AuthenticationProof` | `request_id`, `client_proof` (bytes, 32 bytes) |
| 503 | `AuthenticationResult` | `request_id`, `outcome` (enum), `server_signature` (bytes, 32 bytes), `force_password_change` (bool) |

`request_id` carries the gateway's `ConnectionID` for the FIX session unchanged through
all four messages so the gateway can correlate the result with the correct pending session.

### Protocol Flow

```
FIX client sends Logon
    │
    ▼
Order Gateway
    sends AuthenticationRequest(request_id=conn_id, comp_id, client_nonce)
    │  PDU 500 (port 7070)
    ▼
Authentication Service
    looks up comp_id credential
    generates server_nonce, salt, iterations
    sends AuthenticationChallenge
    │  PDU 501
    ▼
Order Gateway
    computes ClientProof (see below)
    sends AuthenticationProof(client_proof)
    │  PDU 502
    ▼
Authentication Service
    verifies ClientProof against StoredKey
    sends AuthenticationResult(outcome, server_signature)
    │  PDU 503
    ▼
Order Gateway
    verifies ServerSignature — confirms service is genuine
    on success: completes FIX Logon
    on failure: sends FIX Logout, disconnects
```

### SCRAM Computation

```
SaltedPassword = PBKDF2-SHA256(password, salt, iterations)
ClientKey      = HMAC-SHA256(SaltedPassword, "Client Key")
StoredKey      = SHA256(ClientKey)
ServerKey      = HMAC-SHA256(SaltedPassword, "Server Key")
AuthMessage    = uint32le(len(comp_id)) || comp_id
               || uint32le(len(client_nonce)) || client_nonce
               || uint32le(len(server_nonce)) || server_nonce
               || uint32le(len(salt)) || salt
               || uint32le(iterations)
ClientSig      = HMAC-SHA256(StoredKey, AuthMessage)
ClientProof    = ClientKey XOR ClientSig        ← sent in AuthenticationProof
ServerSig      = HMAC-SHA256(ServerKey, AuthMessage)  ← verified by gateway
```

The server stores only `StoredKey` and `ServerKey` — never the plaintext password or
`SaltedPassword`.

### ScramCrypto Library

`libraries/scram_crypto/` — a static library linked by both the authentication service and
the gateway. Namespace `scram_crypto`. Free functions:

| Function | Purpose |
|----------|---------|
| `hmac_sha256` | HMAC-SHA256 over arbitrary bytes |
| `sha256` | SHA-256 hash |
| `pbkdf2_sha256` | PBKDF2-SHA256 key derivation |
| `make_scram_credential` | Derives `StoredKey` + `ServerKey` from a plaintext password |
| `compute_auth_message` | Assembles the canonical `AuthMessage` byte string |

Depends on `OpenSSL::Crypto` (PRIVATE linkage).

### Credential Storage

Credentials (`StoredKey`, `ServerKey`, `salt`, `iterations`) are managed in PostgreSQL by
the Java admin service (`java/admin-service/`) via plain JDBC. The
`db/export_credentials.py` script exports credentials to `credentials.toml` for the
authentication service to load at startup.

The authentication hot path (SCRAM exchange) never queries the database. All credentials
are pre-loaded at startup into an `unordered_map<string, ScramCredential>` held in
`AuthenticationThread`. On SIGHUP or an admin PDU, credentials are reloaded without
restarting the service.

---

## Relationship Between TLS and SCRAM

| Concern | Mechanism |
|---------|-----------|
| Channel confidentiality and integrity | TLS |
| Client identity proof (knows the password) | SCRAM `ClientProof` |
| Server identity proof (genuine service) | SCRAM `ServerSignature` verified by gateway |

TLS prevents a network eavesdropper from observing or tampering with the SCRAM exchange.
SCRAM prevents an impostor authentication service from fooling the gateway (the gateway
checks `ServerSignature`). In a production deployment both are required. During internal
development the SCRAM exchange travels over a plaintext TCP connection, which is acceptable
for localhost but not for any externally-exposed endpoint.

---

## See Also

- [Socket Communications](socket_comms.md) — `ProtocolHandlerInterface`, `RawBytesProtocolHandler`, `MirroredBuffer`
- [Order Gateway](../applications/order_gateway.md) — how the gateway initiates SCRAM and handles the result
