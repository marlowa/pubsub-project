# -----------------------------------------------------------------------------
#  authentication.dsl
#
#  PDU protocol between the gateway and the authentication service.
#
#  The authentication service is a standalone process. The gateway connects
#  to it as a TLS client. The connection carries PDU-framed messages defined
#  here. No FIX protocol concepts appear anywhere in this file — the
#  authentication service is protocol-agnostic; it is the gateway's
#  responsibility to extract credentials from whichever client protocol it
#  speaks and map them onto this protocol.
#
#  Authentication mechanism: SCRAM-SHA-256 (RFC 5802)
#
#  SCRAM avoids transmitting the plaintext password over the wire. The
#  gateway and authentication service exchange a four-message challenge-
#  response sequence. Only a derived proof travels on the wire; the
#  authentication service stores only a salted hash and never sees the
#  plaintext password.
#
#  The binary fields (client_nonce, server_nonce, salt, client_proof,
#  server_signature) are carried as DSL bytes fields: length-prefixed raw
#  byte sequences. No base64 encoding is needed; the connection is
#  TLS-protected and bytes carries arbitrary binary content directly.
#
#  Exchange sequence:
#
#    Gateway                           Authentication Service
#       |                                        |
#       |--- AuthenticationRequest ------------->|  comp_id + client nonce
#       |<-- AuthenticationChallenge ------------|  server nonce, salt, iterations
#       |    (gateway computes client proof)     |
#       |--- AuthenticationProof -------------->|  client proof
#       |<-- AuthenticationResult --------------|  outcome + server signature
#       |    (gateway verifies server signature) |
#
#  Request correlation:
#    Each gateway may have multiple logons in flight simultaneously (one per
#    incoming FIX session). The gateway assigns a request_id to each
#    AuthenticationRequest and the authentication service echoes it in all
#    subsequent messages for that exchange. The request_id is opaque to the
#    authentication service; it is the gateway's correlation handle.
#
#  ID range:
#    500-599 reserved for the authentication service protocol.
#    100-401 are used by the leader-follower protocol.
#    1000+   are used by the equity order topics.
#
# -----------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# AuthenticationOutcome
# ---------------------------------------------------------------------------

enum AuthenticationOutcome : i32 {
    Granted             = 0
    BadPassword         = 1
    AccountLocked       = 2
    AccountDisabled     = 3
    PasswordExpired     = 4
    ForcePasswordChange = 5
    ServiceUnavailable  = 6
    UnknownUser         = 7
}

# ---------------------------------------------------------------------------
# AuthenticationRequest (id=500)
#
# Sent by the gateway to initiate authentication for one logon attempt.
# The client_nonce is a cryptographically random byte sequence generated
# by the gateway for this specific exchange; it must not be reused.
# ---------------------------------------------------------------------------

message AuthenticationRequest (id=500, version=1)
    i64    request_id      # gateway-assigned correlation handle, echoed in all replies
    string comp_id         # the identity being authenticated
    bytes  client_nonce    # random bytes, generated fresh per attempt by the gateway
end

# ---------------------------------------------------------------------------
# AuthenticationChallenge (id=501)
#
# Sent by the authentication service in response to AuthenticationRequest.
# The server_nonce is the client_nonce with a server-generated suffix
# appended, as specified by RFC 5802.
# salt and iterations are the parameters stored for this comp_id and are
# needed by the gateway to derive the client proof via PBKDF2.
# ---------------------------------------------------------------------------

message AuthenticationChallenge (id=501, version=1)
    i64    request_id      # echoed from AuthenticationRequest
    bytes  server_nonce    # client_nonce with server-generated suffix appended
    bytes  salt            # the stored salt for this comp_id
    i32    iterations      # PBKDF2 iteration count stored for this comp_id
end

# ---------------------------------------------------------------------------
# AuthenticationProof (id=502)
#
# Sent by the gateway after it has derived the SCRAM client proof using
# the password, salt, iterations, and both nonces from the challenge.
# The plaintext password never leaves the gateway.
# ---------------------------------------------------------------------------

message AuthenticationProof (id=502, version=1)
    i64    request_id      # echoed from AuthenticationRequest
    bytes  client_proof    # SCRAM-SHA-256 ClientProof as defined by RFC 5802
end

# ---------------------------------------------------------------------------
# AuthenticationResult (id=503)
#
# The final message in the exchange, sent by the authentication service.
# server_signature is the SCRAM ServerSignature as defined by RFC 5802.
# The gateway must verify it to confirm it is speaking to the genuine
# authentication service and not an impostor (mutual authentication).
# server_signature is only meaningful when outcome is Granted or
# ForcePasswordChange; it is empty for all other outcomes.
# ---------------------------------------------------------------------------

message AuthenticationResult (id=503, version=1)
    i64                   request_id              # echoed from AuthenticationRequest
    AuthenticationOutcome outcome
    bytes                 server_signature        # SCRAM ServerSignature; empty on failure
    bool                  force_password_change   # true when outcome is Granted but the credential must be renewed
end
