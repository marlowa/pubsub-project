package com.pubsub.admin.service;

import com.pubsub.admin.protocol.Authentication;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Sends credential management PDUs to the authentication service TLS admin channel.
 */
public class AuthServiceClient {
    private static final Logger log = LoggerFactory.getLogger(AuthServiceClient.class);

    private final String host;
    private final int port;
    private final AtomicLong requestIdCounter = new AtomicLong(1);

    public AuthServiceClient(String host, int port) {
        this.host = host;
        this.port = port;
    }

    public void setCredential(String compId, String password, int iterations) throws IOException {
        log.info("setCredential: compId={} iterations={} -> {}:{}", compId, iterations, host, port);
        Authentication.SetCredentialRequest req = new Authentication.SetCredentialRequest();
        req.request_id = requestIdCounter.getAndIncrement();
        req.comp_id = compId;
        req.password = password;
        req.iterations = iterations;

        ByteBuffer buf = ByteBuffer.allocate(Authentication.SetCredentialRequest.encodedSize(req));
        Authentication.SetCredentialRequest.encode(req, buf);

        try (PduChannel channel = new PduChannel(host, port,
                Authentication.PDU_HEADER_SIZE, Authentication::writeHeader, Authentication::readHeader)) {
            log.debug("setCredential: TLS connected, sending SetCredentialRequest request_id={}", req.request_id);
            channel.send(Authentication.SetCredentialRequest.PDU_ID, buf.array());
            log.debug("setCredential: awaiting SetCredentialResult");
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
            log.info("setCredential: compId={} -- Success", compId);
        }
    }

    public void removeCredential(String compId) throws IOException {
        log.info("removeCredential: compId={} -> {}:{}", compId, host, port);
        Authentication.RemoveCredentialRequest req = new Authentication.RemoveCredentialRequest();
        req.request_id = requestIdCounter.getAndIncrement();
        req.comp_id = compId;

        ByteBuffer buf = ByteBuffer.allocate(Authentication.RemoveCredentialRequest.encodedSize(req));
        Authentication.RemoveCredentialRequest.encode(req, buf);

        try (PduChannel channel = new PduChannel(host, port,
                Authentication.PDU_HEADER_SIZE, Authentication::writeHeader, Authentication::readHeader)) {
            channel.send(Authentication.RemoveCredentialRequest.PDU_ID, buf.array());
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
    }

    public void restoreCredential(String compId, ScramCredential cred) throws IOException {
        log.info("restoreCredential: compId={} -> {}:{}", compId, host, port);
        Authentication.RestoreCredentialRequest req = new Authentication.RestoreCredentialRequest();
        req.request_id = requestIdCounter.getAndIncrement();
        req.comp_id = compId;
        req.stored_key = hexToBytes(cred.storedKey());
        req.server_key = hexToBytes(cred.serverKey());
        req.salt = hexToBytes(cred.salt());
        req.iterations = cred.iterations();

        ByteBuffer buf = ByteBuffer.allocate(Authentication.RestoreCredentialRequest.encodedSize(req));
        Authentication.RestoreCredentialRequest.encode(req, buf);

        try (PduChannel channel = new PduChannel(host, port,
                Authentication.PDU_HEADER_SIZE, Authentication::writeHeader, Authentication::readHeader)) {
            channel.send(Authentication.RestoreCredentialRequest.PDU_ID, buf.array());
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
