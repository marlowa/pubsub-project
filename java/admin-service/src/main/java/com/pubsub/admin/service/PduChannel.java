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
import java.security.cert.X509Certificate;

/**
 * A single-use TLS connection that sends and receives framed PDUs.
 * Knows nothing about the wire format: header encoding and decoding are
 * delegated to the HeaderWriter and HeaderReader supplied by the caller,
 * which are generated from the DSL framing block.
 */
public class PduChannel implements AutoCloseable {

    @FunctionalInterface
    public interface HeaderWriter {
        void write(ByteBuffer buf, int pduId, int payloadLength);
    }

    @FunctionalInterface
    public interface HeaderReader {
        /** Returns [pduId, payloadLength]. Throws if the canary is wrong. */
        int[] read(ByteBuffer buf) throws IOException;
    }

    private final int headerSize;
    private final HeaderWriter headerWriter;
    private final HeaderReader headerReader;

    private final SSLSocket socket;
    private final DataInputStream in;
    private final OutputStream out;

    public PduChannel(String host, int port, int headerSize,
                      HeaderWriter headerWriter, HeaderReader headerReader)
            throws IOException {
        this.headerSize = headerSize;
        this.headerWriter = headerWriter;
        this.headerReader = headerReader;
        this.socket = connectTls(host, port);
        this.in = new DataInputStream(new BufferedInputStream(socket.getInputStream()));
        this.out = socket.getOutputStream();
    }

    public void send(int pduId, byte[] payload) throws IOException {
        ByteBuffer header = ByteBuffer.allocate(headerSize);
        headerWriter.write(header, pduId, payload.length);
        out.write(header.array());
        out.write(payload);
        out.flush();
    }

    public byte[] receive(int expectedPduId) throws IOException {
        byte[] headerBytes = new byte[headerSize];
        in.readFully(headerBytes);
        int[] decoded = headerReader.read(ByteBuffer.wrap(headerBytes));
        int pduId = decoded[0];
        int payloadLength = decoded[1];
        if (pduId != expectedPduId) {
            throw new IOException("Expected PDU ID " + expectedPduId + ", got " + pduId);
        }
        byte[] payload = new byte[payloadLength];
        in.readFully(payload);
        return payload;
    }

    @Override
    public void close() {
        try {
            socket.close();
        } catch (IOException ignored) {
        }
    }

    private static SSLSocket connectTls(String host, int port) throws IOException {
        try {
            TrustManager[] trustAll = new TrustManager[]{
                new X509TrustManager() {
                    public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
                    public void checkClientTrusted(X509Certificate[] certs, String authType) {}
                    public void checkServerTrusted(X509Certificate[] certs, String authType) {}
                }
            };
            SSLContext ctx = SSLContext.getInstance("TLS");
            ctx.init(null, trustAll, null);
            SSLSocket sock = (SSLSocket) ctx.getSocketFactory().createSocket(host, port);
            sock.startHandshake();
            return sock;
        } catch (Exception e) {
            throw new IOException("TLS connection to " + host + ":" + port + " failed", e);
        }
    }
}
