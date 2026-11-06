package com.pubsub.fixtestclient.fix;

import org.junit.jupiter.api.Test;
import quickfix.DefaultMessageFactory;
import quickfix.MemoryStoreFactory;
import quickfix.SLF4JLogFactory;
import quickfix.Session;
import quickfix.SessionID;
import quickfix.SessionSettings;
import quickfix.SocketInitiator;

import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class FixTransportIntegrationTest {

    @Test
    void transportDisconnectClearsConnectedAndSetsGenericError() throws Exception {
        try (ServerSocket gatewaySocket = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
            gatewaySocket.setSoTimeout(5000);
            SessionID sessionId = new SessionID("FIXT.1.1", "CLIENT" + UUID.randomUUID(), "GATEWAY");
            SessionSettings settings = settings(sessionId, gatewaySocket.getLocalPort());
            FixApplication application = new FixApplication();
            SocketInitiator initiator = new SocketInitiator(
                    application,
                    new MemoryStoreFactory(),
                    settings,
                    new SLF4JLogFactory(settings),
                    new DefaultMessageFactory());
            Socket gatewayConnection = null;

            try {
                initiator.start();
                gatewayConnection = gatewaySocket.accept();

                assertTrue(waitUntil(application::isTransportConnected));
                Session session = Session.lookupSession(sessionId);
                assertNotNull(session);
                assertFalse(session.isLoggedOn());

                gatewayConnection.close();
                gatewayConnection = null;

                assertTrue(waitUntil(() -> !application.isTransportConnected()));
                assertEquals("FIX transport disconnected", application.getLastLogoutReason());
            } finally {
                if (gatewayConnection != null) {
                    gatewayConnection.close();
                }
                application.stopObservingTransport();
                initiator.stop();
            }
        }
    }

    private static SessionSettings settings(SessionID sessionId, int port) {
        SessionSettings settings = new SessionSettings();
        settings.setString("ConnectionType", "initiator");
        settings.setString("HeartBtInt", "30");
        settings.setString("DefaultApplVerID", "FIX.5.0SP2");
        settings.setString("TransportDataDictionary", "FIXT11.xml");
        settings.setString("AppDataDictionary", "FIX50SP2.xml");
        settings.setString("StartTime", "00:00:00");
        settings.setString("EndTime", "00:00:00");
        settings.setString("ReconnectInterval", "30");
        settings.setString(sessionId, "BeginString", "FIXT.1.1");
        settings.setString(sessionId, "SocketConnectHost", "127.0.0.1");
        settings.setString(sessionId, "SocketConnectPort", String.valueOf(port));
        return settings;
    }

    private static boolean waitUntil(BooleanSupplier condition) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.sleep(10);
        }
        return condition.getAsBoolean();
    }
}
