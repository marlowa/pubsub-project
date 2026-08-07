package com.pubsub.fixtestclient.binary;

import com.pubsub.fixtestclient.Config;
import com.pubsub.fixtestclient.blotter.BlotterStore;
import com.pubsub.fixtestclient.gateway.GatewayEndpoint;
import com.pubsub.fixtestclient.fix.SessionStatus;
import com.pubsub.fixtestclient.protocol.BinarySession;
import com.pubsub.fixtestclient.protocol.FixOrders;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.time.Instant;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Session with the binary gateway: the peer of {@link com.pubsub.fixtestclient.fix.FixEngine},
 * for the venue's other front door.
 *
 * Much smaller than its FIX counterpart, because there is no session layer to run. The
 * gateway speaks framed PDUs, so the framework already delivers whole messages in order and
 * notices a dead connection; there are no sequence numbers to track, no heartbeats to answer
 * and no resend logic. What is left is a socket, a Logon, and a thread reading reports.
 *
 * The wire types come from the same DSL the C++ side is generated from, so this client
 * cannot drift from the gateway it talks to.
 */
public final class BinaryEngine {

    private static final Logger log = LoggerFactory.getLogger(BinaryEngine.class);
    private static final int CONNECT_TIMEOUT_MILLIS = 5000;
    private static final int LOGON_TIMEOUT_MILLIS = 10000;

    private final Config config;
    private final BlotterStore blotterStore;

    // Which endpoint the live session is on. Reported in the status rather than read back
    // from configuration, so the page names the gateway actually connected to -- with two
    // instances per protocol, the config alone no longer answers that.
    private volatile GatewayEndpoint endpoint;
    private final AtomicLong orderCounter = new AtomicLong(1);

    private volatile Socket socket;
    private volatile DataInputStream input;
    private volatile OutputStream output;
    private volatile Thread receiver;

    private volatile boolean loggedOn;
    private volatile String compId = "";
    private volatile Instant logonTime;
    private volatile String lastError = "";

    public BinaryEngine(Config config, BlotterStore blotterStore) {
        this.config = config;
        this.blotterStore = blotterStore;
    }

    /**
     * Connects and logs on, returning only once the gateway has accepted or refused.
     *
     * Synchronous by design: the caller is a web request handler that must tell the user
     * whether the logon worked, and a binary logon is a single round trip.
     *
     * @param endpoint    which binary gateway to connect to -- the venue runs more than one
     * @param compIdToUse the identity to log on with, as SenderCompID is in FIX
     * @param password    verified by SCRAM at the gateway; never sent onwards from there
     * @param targetCompId the venue the client expects, or empty to skip the check
     * @return an empty string on success, or the reason it failed
     */
    public synchronized String logon(GatewayEndpoint endpoint, String compIdToUse, String password, String targetCompId) {
        if (loggedOn) {
            return "already logged on as " + compId;
        }
        if (compIdToUse == null || compIdToUse.isBlank()) {
            return "comp id is required";
        }

        this.endpoint = endpoint;
        try {
            Socket newSocket = new Socket();
            newSocket.connect(new InetSocketAddress(endpoint.host(), endpoint.port()), CONNECT_TIMEOUT_MILLIS);
            newSocket.setTcpNoDelay(true);
            newSocket.setSoTimeout(LOGON_TIMEOUT_MILLIS);

            this.socket = newSocket;
            this.input = new DataInputStream(newSocket.getInputStream());
            this.output = newSocket.getOutputStream();

            BinarySession.Logon logonMessage = new BinarySession.Logon();
            logonMessage.comp_id = compIdToUse.trim();
            logonMessage.password = password == null ? "" : password;
            // Empty means "I do not mind which venue"; the gateway checks a populated one
            // against its own identity and refuses a mismatch.
            logonMessage.target_comp_id = targetCompId == null ? "" : targetCompId.trim();
            sendPdu(BinarySession.Logon.PDU_ID, BinarySession.Logon.encodedSize(logonMessage),
                    buffer -> BinarySession.Logon.encode(logonMessage, buffer));

            Frame frame = readFrame();
            if (frame == null || frame.pduId != BinarySession.LogonAck.PDU_ID) {
                return closeWithError("no LogonAck from the gateway");
            }
            BinarySession.LogonAck ack = BinarySession.LogonAck.decode(ByteBuffer.wrap(frame.payload));
            if (ack == null) {
                return closeWithError("LogonAck could not be decoded");
            }
            if (ack.outcome != BinarySession.LogonOutcome.Accepted) {
                return closeWithError("logon refused: " + ack.outcome
                                      + (ack.has_text ? " (" + ack.text + ")" : ""));
            }

            // Blocking reads from here: the receiver waits for reports rather than spinning,
            // and is stopped by closing the socket.
            newSocket.setSoTimeout(0);
            this.compId = compIdToUse.trim();
            this.loggedOn = true;
            this.logonTime = Instant.now();
            this.lastError = "";

            this.receiver = new Thread(this::receiveLoop, "binary-gateway-receiver");
            this.receiver.setDaemon(true);
            this.receiver.start();

            log.info("Binary gateway session established as {}", compId);
            return "";
        } catch (IOException e) {
            return closeWithError("connect failed: " + e.getMessage());
        }
    }

    /** Closes the session. The binary protocol has no Logout message: the socket is the session. */
    public synchronized void logout() {
        loggedOn = false;
        closeSocket();
        Thread current = receiver;
        if (current != null) {
            try {
                current.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            receiver = null;
        }
        log.info("Binary gateway session closed");
    }

    public boolean isLoggedOn() {
        return loggedOn;
    }

    public SessionStatus getStatus() {
        if (!loggedOn) {
            return SessionStatus.disconnected(lastError);
        }
        // Sequence numbers are FIX session-layer concepts with no binary equivalent, so they
        // are reported as zero rather than invented.
        return new SessionStatus(true, false, true, compId, "BINARY-GATEWAY",
                                 endpoint != null ? endpoint.host() : "",
                                 endpoint != null ? endpoint.port() : 0,
                                 logonTime, 0, 0, 0, lastError, 0);
    }

    /** Sends a NewOrderSingle, returning an empty string on success or the reason it failed. */
    public String sendNewOrderSingle(FixOrders.NewOrderSingle order) {
        if (!loggedOn) {
            return "not logged on to the binary gateway";
        }
        try {
            sendPdu(FixOrders.NewOrderSingle.PDU_ID, FixOrders.NewOrderSingle.encodedSize(order),
                    buffer -> FixOrders.NewOrderSingle.encode(order, buffer));
            blotterStore.add(BinaryBlotterRows.fromOrder(blotterStore.nextRowId(), order));
            return "";
        } catch (IOException e) {
            return "send failed: " + e.getMessage();
        }
    }

    /** Sends an OrderCancelRequest, returning an empty string on success or the reason it failed. */
    public String sendOrderCancelRequest(FixOrders.OrderCancelRequest cancel) {
        if (!loggedOn) {
            return "not logged on to the binary gateway";
        }
        try {
            sendPdu(FixOrders.OrderCancelRequest.PDU_ID, FixOrders.OrderCancelRequest.encodedSize(cancel),
                    buffer -> FixOrders.OrderCancelRequest.encode(cancel, buffer));
            blotterStore.add(BinaryBlotterRows.fromCancel(blotterStore.nextRowId(), cancel));
            return "";
        } catch (IOException e) {
            return "send failed: " + e.getMessage();
        }
    }

    /** Generates a ClOrdID unique to this session, for the order form's convenience. */
    public String nextClOrdId() {
        return compId + "-" + System.currentTimeMillis() + "-" + orderCounter.getAndIncrement();
    }

    @FunctionalInterface
    private interface PayloadEncoder {
        int encode(ByteBuffer buffer);
    }

    private void sendPdu(int pduId, int payloadSize, PayloadEncoder encoder) throws IOException {
        // Each codec sets the byte order it needs: the header is network order, the payload
        // is the DSL's little-endian. Setting one here would be overwritten by the other.
        ByteBuffer frame = ByteBuffer.allocate(BinarySession.PDU_HEADER_SIZE + payloadSize);
        BinarySession.writeHeader(frame, pduId, payloadSize);
        encoder.encode(frame);

        OutputStream stream = output;
        if (stream == null) {
            throw new IOException("not connected");
        }
        synchronized (this) {
            stream.write(frame.array(), 0, frame.position());
            stream.flush();
        }
    }

    private static final class Frame {
        private final int pduId;
        private final byte[] payload;

        private Frame(int pduId, byte[] payload) {
            this.pduId = pduId;
            this.payload = payload;
        }
    }

    private Frame readFrame() throws IOException {
        DataInputStream stream = input;
        if (stream == null) {
            return null;
        }
        byte[] headerBytes = new byte[BinarySession.PDU_HEADER_SIZE];
        stream.readFully(headerBytes);
        int[] decoded = BinarySession.readHeader(ByteBuffer.wrap(headerBytes));
        byte[] payload = new byte[decoded[1]];
        if (payload.length > 0) {
            stream.readFully(payload);
        }
        return new Frame(decoded[0], payload);
    }

    private void receiveLoop() {
        try {
            while (loggedOn) {
                Frame frame = readFrame();
                if (frame == null) {
                    return;
                }
                if (frame.pduId != FixOrders.ExecutionReport.PDU_ID) {
                    log.debug("Ignoring PDU id {} from the binary gateway", frame.pduId);
                    continue;
                }
                FixOrders.ExecutionReport report = FixOrders.ExecutionReport.decode(ByteBuffer.wrap(frame.payload));
                if (report == null) {
                    log.warn("ExecutionReport from the binary gateway could not be decoded");
                    continue;
                }
                blotterStore.add(BinaryBlotterRows.fromReport(blotterStore.nextRowId(), report));
            }
        } catch (IOException e) {
            if (loggedOn) {
                // Only a surprise if we did not close the socket ourselves.
                lastError = "connection lost: " + e.getMessage();
                loggedOn = false;
                log.warn("Binary gateway connection lost: {}", e.getMessage());
            }
        }
    }

    private String closeWithError(String reason) {
        lastError = reason;
        loggedOn = false;
        closeSocket();
        log.warn("Binary gateway logon failed: {}", reason);
        return reason;
    }

    private void closeSocket() {
        Socket current = socket;
        if (current != null) {
            try {
                current.close();
            } catch (IOException ignored) {
                // Closing a socket that is already gone is not a failure worth reporting.
            }
        }
        socket = null;
        input = null;
        output = null;
    }

}
