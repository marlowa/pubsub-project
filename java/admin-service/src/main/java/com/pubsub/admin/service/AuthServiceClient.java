package com.pubsub.admin.service;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;
import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.cert.X509Certificate;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Sends credential management PDUs to the authentication service TLS admin channel.
 * Protocol: 24-byte big-endian header followed by a little-endian encoded payload.
 *
 * PDU 510 SetCredentialRequest   / PDU 511 SetCredentialResult
 * PDU 512 RemoveCredentialRequest / PDU 513 RemoveCredentialResult
 */
public class AuthServiceClient {
    private static final int PDU_HEADER_SIZE = 24;
    private static final int PDU_CANARY = 0xC0FFEE00;
    private static final int PDU_VERSION = 1;
    private static final int PDU_ID_SET_CREDENTIAL_REQUEST    = 510;
    private static final int PDU_ID_SET_CREDENTIAL_RESULT     = 511;
    private static final int PDU_ID_REMOVE_CREDENTIAL_REQUEST = 512;
    private static final int PDU_ID_REMOVE_CREDENTIAL_RESULT  = 513;
    private static final int OUTCOME_SUCCESS = 0;

    private final String host;
    private final int port;
    private final AtomicLong requestIdCounter = new AtomicLong(1);

    public AuthServiceClient(String host, int port) {
        this.host = host;
        this.port = port;
    }

    public void setCredential(String compId, String password, int iterations) throws IOException {
        long requestId = requestIdCounter.getAndIncrement();
        try (SSLSocket socket = connectTls()) {
            DataInputStream in = new DataInputStream(
                    new BufferedInputStream(socket.getInputStream()));
            OutputStream out = socket.getOutputStream();

            byte[] payload = encodeSetCredentialRequest(requestId, compId, password, iterations);
            sendPdu(out, PDU_ID_SET_CREDENTIAL_REQUEST, payload);

            int[] pduIdOut = new int[1];
            byte[] responsePayload = recvPdu(in, pduIdOut);
            if (pduIdOut[0] != PDU_ID_SET_CREDENTIAL_RESULT) {
                throw new IOException("Expected PDU " + PDU_ID_SET_CREDENTIAL_RESULT
                        + " (SetCredentialResult), got " + pduIdOut[0]);
            }
            decodeAndValidateResult(responsePayload, requestId, compId);
        }
    }

    public void removeCredential(String compId) throws IOException {
        long requestId = requestIdCounter.getAndIncrement();
        try (SSLSocket socket = connectTls()) {
            DataInputStream in = new DataInputStream(
                    new BufferedInputStream(socket.getInputStream()));
            OutputStream out = socket.getOutputStream();

            byte[] payload = encodeRemoveCredentialRequest(requestId, compId);
            sendPdu(out, PDU_ID_REMOVE_CREDENTIAL_REQUEST, payload);

            int[] pduIdOut = new int[1];
            byte[] responsePayload = recvPdu(in, pduIdOut);
            if (pduIdOut[0] != PDU_ID_REMOVE_CREDENTIAL_RESULT) {
                throw new IOException("Expected PDU " + PDU_ID_REMOVE_CREDENTIAL_RESULT
                        + " (RemoveCredentialResult), got " + pduIdOut[0]);
            }
            decodeAndValidateRemoveResult(responsePayload, requestId, compId);
        }
    }

    private void decodeAndValidateResult(byte[] payload, long requestId, String compId)
            throws IOException {
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        long responseRequestId = buf.getLong();
        if (responseRequestId != requestId) {
            throw new IOException("SetCredentialResult request_id mismatch: expected "
                    + requestId + ", got " + responseRequestId);
        }
        int compIdLen = buf.getInt();
        byte[] compIdBytes = new byte[compIdLen];
        buf.get(compIdBytes);
        int outcome = buf.getInt();
        if (outcome != OUTCOME_SUCCESS) {
            throw new IOException("SetCredentialResult outcome=" + outcome
                    + " for compId='" + compId + "' (expected 0=Success)");
        }
    }

    private void decodeAndValidateRemoveResult(byte[] payload, long requestId, String compId)
            throws IOException {
        ByteBuffer buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        long responseRequestId = buf.getLong();
        if (responseRequestId != requestId) {
            throw new IOException("RemoveCredentialResult request_id mismatch: expected "
                    + requestId + ", got " + responseRequestId);
        }
        int compIdLen = buf.getInt();
        byte[] compIdBytes = new byte[compIdLen];
        buf.get(compIdBytes);
        int outcome = buf.getInt();
        // 0=Success, 1=NotFound — both are acceptable (NotFound means already absent)
        if (outcome != OUTCOME_SUCCESS && outcome != 1) {
            throw new IOException("RemoveCredentialResult outcome=" + outcome
                    + " for compId='" + compId + "'");
        }
    }

    private SSLSocket connectTls() throws IOException {
        try {
            TrustManager[] trustAll = new TrustManager[]{
                new X509TrustManager() {
                    public X509Certificate[] getAcceptedIssuers() {
                        return new X509Certificate[0];
                    }
                    public void checkClientTrusted(X509Certificate[] certs, String authType) {}
                    public void checkServerTrusted(X509Certificate[] certs, String authType) {}
                }
            };
            SSLContext ctx = SSLContext.getInstance("TLS");
            ctx.init(null, trustAll, null);
            SSLSocket socket = (SSLSocket) ctx.getSocketFactory().createSocket(host, port);
            socket.startHandshake();
            return socket;
        } catch (Exception e) {
            throw new IOException("TLS connection to " + host + ":" + port + " failed", e);
        }
    }

    private byte[] encodeRemoveCredentialRequest(long requestId, String compId) {
        byte[] compIdBytes = compId.getBytes(StandardCharsets.UTF_8);
        int size = 8 + 4 + compIdBytes.length;
        ByteBuffer buf = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        buf.putLong(requestId);
        buf.putInt(compIdBytes.length);
        buf.put(compIdBytes);
        return buf.array();
    }

    private byte[] encodeSetCredentialRequest(long requestId, String compId,
                                              String password, int iterations) {
        byte[] compIdBytes = compId.getBytes(StandardCharsets.UTF_8);
        byte[] passwordBytes = password.getBytes(StandardCharsets.UTF_8);
        int size = 8 + 4 + compIdBytes.length + 4 + passwordBytes.length + 4;
        ByteBuffer buf = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        buf.putLong(requestId);
        buf.putInt(compIdBytes.length);
        buf.put(compIdBytes);
        buf.putInt(passwordBytes.length);
        buf.put(passwordBytes);
        buf.putInt(iterations);
        return buf.array();
    }

    private void sendPdu(OutputStream out, int pduId, byte[] payload) throws IOException {
        ByteBuffer header = ByteBuffer.allocate(PDU_HEADER_SIZE).order(ByteOrder.BIG_ENDIAN);
        header.putInt(payload.length);
        header.putShort((short) pduId);
        header.put((byte) PDU_VERSION);
        header.put((byte) 0);
        header.putLong(0L);
        header.putInt(PDU_CANARY);
        header.putInt(0);
        out.write(header.array());
        out.write(payload);
        out.flush();
    }

    private byte[] recvPdu(DataInputStream in, int[] pduIdOut) throws IOException {
        byte[] headerBytes = new byte[PDU_HEADER_SIZE];
        in.readFully(headerBytes);
        ByteBuffer header = ByteBuffer.wrap(headerBytes).order(ByteOrder.BIG_ENDIAN);
        int payloadLength = header.getInt();
        pduIdOut[0] = Short.toUnsignedInt(header.getShort());
        header.get();
        header.get();
        header.getLong();
        int canary = header.getInt();
        if (canary != PDU_CANARY) {
            throw new IOException(String.format(
                    "PDU canary mismatch: expected 0x%08X, got 0x%08X", PDU_CANARY, canary));
        }
        byte[] payloadBytes = new byte[payloadLength];
        in.readFully(payloadBytes);
        return payloadBytes;
    }
}
