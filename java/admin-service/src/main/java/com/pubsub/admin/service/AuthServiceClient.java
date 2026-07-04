package com.pubsub.admin.service;

import com.pubsub.admin.protocol.Authentication;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Sends credential-management PDUs to the authentication service TLS admin channel.
 *
 * The authentication service runs as an active/active pair (both instances serve
 * the gateway; both must hold the same in-memory credential set so a failover to
 * either is correct). The admin service is the single writer of credential state:
 * it updates the database and then fans each credential-management PDU out to
 * <em>every</em> auth endpoint so both in-memory caches stay in sync.
 *
 * Fan-out is best-effort: an operation succeeds if at least one endpoint applied
 * it (the database is already the durable source of truth, and an endpoint that
 * missed the PDU reconciles from the DB export on its next start). It fails only
 * if every endpoint is unreachable. Partial failures are logged as warnings.
 */
public class AuthServiceClient {
    private static final Logger log = LoggerFactory.getLogger(AuthServiceClient.class);

    /** A single auth-service admin endpoint. */
    public record Endpoint(String host, int port) {
        @Override
        public String toString() {
            return host + ":" + port;
        }
    }

    @FunctionalInterface
    private interface EndpointAction {
        void run(Endpoint endpoint) throws IOException;
    }

    private final List<Endpoint> endpoints;
    private final AtomicLong requestIdCounter = new AtomicLong(1);

    public AuthServiceClient(List<Endpoint> endpoints) {
        this.endpoints = List.copyOf(endpoints);
    }

    /** Convenience single-endpoint constructor (used by tests). */
    public AuthServiceClient(String host, int port) {
        this(List.of(new Endpoint(host, port)));
    }

    /**
     * Run {@code action} against every endpoint, collecting per-endpoint failures.
     * Throws only if all endpoints fail; warns on partial failure.
     */
    private void fanOut(String op, String compId, EndpointAction action) throws IOException {
        if (endpoints.isEmpty()) {
            return;
        }
        int successes = 0;
        List<String> failures = new ArrayList<>();
        for (Endpoint endpoint : endpoints) {
            try {
                action.run(endpoint);
                successes++;
                log.info("{}: compId={} -- Success on {}", op, compId, endpoint);
            } catch (IOException e) {
                failures.add(endpoint + " (" + e.getMessage() + ")");
                log.warn("{}: compId={} FAILED on {} -- {}", op, compId, endpoint, e.getMessage());
            }
        }
        if (successes == 0) {
            throw new IOException(op + " failed for compId='" + compId + "' on all "
                    + endpoints.size() + " auth endpoint(s): " + failures);
        }
        if (!failures.isEmpty()) {
            log.warn("{}: compId={} applied to {}/{} auth endpoint(s); {} unreachable "
                    + "(will reconcile from the DB on restart): {}",
                    op, compId, successes, endpoints.size(), failures.size(), failures);
        }
    }

    public void setCredential(String compId, String password, int iterations) throws IOException {
        Authentication.SetCredentialRequest req = new Authentication.SetCredentialRequest();
        req.request_id = requestIdCounter.getAndIncrement();
        req.comp_id = compId;
        req.password = password;
        req.iterations = iterations;

        ByteBuffer buf = ByteBuffer.allocate(Authentication.SetCredentialRequest.encodedSize(req));
        Authentication.SetCredentialRequest.encode(req, buf);
        byte[] bytes = buf.array();

        fanOut("setCredential", compId, endpoint -> {
            try (PduChannel channel = new PduChannel(endpoint.host(), endpoint.port(),
                    Authentication.PDU_HEADER_SIZE, Authentication::writeHeader, Authentication::readHeader)) {
                channel.send(Authentication.SetCredentialRequest.PDU_ID, bytes);
                byte[] payload = channel.receive(Authentication.SetCredentialResult.PDU_ID);
                Authentication.SetCredentialResult result =
                        Authentication.SetCredentialResult.decode(ByteBuffer.wrap(payload));
                if (result == null) {
                    throw new IOException("Failed to decode SetCredentialResult");
                }
                if (result.request_id != req.request_id) {
                    throw new IOException("SetCredentialResult request_id mismatch: expected "
                            + req.request_id + ", got " + result.request_id);
                }
                if (result.outcome != Authentication.SetCredentialOutcome.Success) {
                    throw new IOException("SetCredentialResult outcome=" + result.outcome
                            + " for compId='" + compId + "' (expected Success)");
                }
            }
        });
    }

    public void removeCredential(String compId) throws IOException {
        Authentication.RemoveCredentialRequest req = new Authentication.RemoveCredentialRequest();
        req.request_id = requestIdCounter.getAndIncrement();
        req.comp_id = compId;

        ByteBuffer buf = ByteBuffer.allocate(Authentication.RemoveCredentialRequest.encodedSize(req));
        Authentication.RemoveCredentialRequest.encode(req, buf);
        byte[] bytes = buf.array();

        fanOut("removeCredential", compId, endpoint -> {
            try (PduChannel channel = new PduChannel(endpoint.host(), endpoint.port(),
                    Authentication.PDU_HEADER_SIZE, Authentication::writeHeader, Authentication::readHeader)) {
                channel.send(Authentication.RemoveCredentialRequest.PDU_ID, bytes);
                byte[] payload = channel.receive(Authentication.RemoveCredentialResult.PDU_ID);
                Authentication.RemoveCredentialResult result =
                        Authentication.RemoveCredentialResult.decode(ByteBuffer.wrap(payload));
                if (result == null) {
                    throw new IOException("Failed to decode RemoveCredentialResult");
                }
                if (result.request_id != req.request_id) {
                    throw new IOException("RemoveCredentialResult request_id mismatch: expected "
                            + req.request_id + ", got " + result.request_id);
                }
                if (result.outcome != Authentication.RemoveCredentialOutcome.Success
                        && result.outcome != Authentication.RemoveCredentialOutcome.NotFound) {
                    throw new IOException("RemoveCredentialResult outcome=" + result.outcome
                            + " for compId='" + compId + "'");
                }
            }
        });
    }

    public void restoreCredential(String compId, ScramCredential cred) throws IOException {
        Authentication.RestoreCredentialRequest req = new Authentication.RestoreCredentialRequest();
        req.request_id = requestIdCounter.getAndIncrement();
        req.comp_id = compId;
        req.stored_key = hexToBytes(cred.storedKey());
        req.server_key = hexToBytes(cred.serverKey());
        req.salt = hexToBytes(cred.salt());
        req.iterations = cred.iterations();

        ByteBuffer buf = ByteBuffer.allocate(Authentication.RestoreCredentialRequest.encodedSize(req));
        Authentication.RestoreCredentialRequest.encode(req, buf);
        byte[] bytes = buf.array();

        fanOut("restoreCredential", compId, endpoint -> {
            try (PduChannel channel = new PduChannel(endpoint.host(), endpoint.port(),
                    Authentication.PDU_HEADER_SIZE, Authentication::writeHeader, Authentication::readHeader)) {
                channel.send(Authentication.RestoreCredentialRequest.PDU_ID, bytes);
                byte[] payload = channel.receive(Authentication.RestoreCredentialResult.PDU_ID);
                Authentication.RestoreCredentialResult result =
                        Authentication.RestoreCredentialResult.decode(ByteBuffer.wrap(payload));
                if (result == null) {
                    throw new IOException("Failed to decode RestoreCredentialResult");
                }
                if (result.request_id != req.request_id) {
                    throw new IOException("RestoreCredentialResult request_id mismatch: expected "
                            + req.request_id + ", got " + result.request_id);
                }
                if (result.outcome != Authentication.RestoreCredentialOutcome.Success) {
                    throw new IOException("RestoreCredentialResult outcome=" + result.outcome
                            + " for compId='" + compId + "' (expected Success)");
                }
            }
        });
    }

    private static byte[] hexToBytes(String hex) {
        int length = hex.length();
        byte[] data = new byte[length / 2];
        for (int i = 0; i < length; i += 2) {
            data[i / 2] = (byte) ((Character.digit(hex.charAt(i), 16) << 4)
                    + Character.digit(hex.charAt(i + 1), 16));
        }
        return data;
    }
}
