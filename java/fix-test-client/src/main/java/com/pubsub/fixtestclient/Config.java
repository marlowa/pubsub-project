package com.pubsub.fixtestclient;

import com.moandjiezana.toml.Toml;
import com.pubsub.fixtestclient.fix.LogonMode;
import com.pubsub.fixtestclient.gateway.GatewayEndpoint;
import com.pubsub.fixtestclient.gateway.GatewayKind;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public record Config(
        int serverPort,
        String outputDir,
        String scriptsDir,
        /**
         * Every address this client may log on to, in the order the logon page offers them.
         *
         * Replaces the single host/port per protocol this used to hold. That shape could not
         * express a second instance of a gateway, so instance b was unreachable from the GUI
         * however the venue was deployed -- the client simply had nowhere to put its port.
         */
        List<GatewayEndpoint> gateways,
        boolean tlsEnabled,
        String targetCompId,
        String trustStorePath,
        String trustStorePassword,
        String logoPath
) {
    /**
     * Copies the endpoint list so that the configuration cannot change under the client.
     *
     * A record keeps whatever list it is handed, and a caller that kept its own reference
     * could then add or remove a gateway while a session is live. Copying here makes the
     * list this record hands out immutable whatever it was given.
     */
    public Config {
        gateways = List.copyOf(gateways);
    }

    public static Config load(String path) {
        Toml toml = new Toml().read(new File(path));
        return new Config(
                toml.getLong("server.port", 8081L).intValue(),
                toml.getString("capture.output_dir", "output"),
                toml.getString("scripts.scripts_dir", "scripts"),
                loadGateways(toml),
                toml.getBoolean("fix.tls_enabled", false),
                toml.getString("fix.target_comp_id", "GATEWAY"),
                toml.getString("fix.trust_store_path", "config/fix_gateway_trust.jks"),
                toml.getString("fix.trust_store_password", "pubsub_dev"),
                toml.getString("web.logo_path", "")
        );
    }

    /** @return the endpoint with this key, or empty when the form named one we do not have. */
    public Optional<GatewayEndpoint> gatewayByKey(String key) {
        return gateways.stream().filter(endpoint -> endpoint.key().equals(key)).findFirst();
    }

    /** @return the first configured endpoint -- what the logon page selects by default. */
    public GatewayEndpoint defaultGateway() {
        return gateways.get(0);
    }

    /**
     * Reads the [[gateway]] array.
     *
     * Deliberately has no default. An empty list is a deployment fault -- a client that can
     * reach nothing is useless, and the alternative of quietly inventing localhost:9879 would
     * reproduce exactly the bug this list exists to fix: a single hardcoded endpoint that
     * happens to be instance a.
     */
    private static List<GatewayEndpoint> loadGateways(Toml toml) {
        List<Toml> tables = toml.getTables("gateway");
        if (tables == null || tables.isEmpty()) {
            throw new IllegalStateException(
                    "no [[gateway]] entries in the client configuration -- there is nothing to log on to. "
                    + "Each entry needs key, label, protocol, host and port; see app.toml.");
        }
        List<GatewayEndpoint> endpoints = new ArrayList<>();
        for (Toml table : tables) {
            String key = required(table, "key");
            String protocol = required(table, "protocol");
            endpoints.add(new GatewayEndpoint(
                    key,
                    table.getString("label", key),
                    GatewayKind.fromFormValue(protocol),
                    required(table, "host"),
                    table.getLong("port").intValue(),
                    // Absent means this endpoint has no TLS listener, which is different from
                    // sharing the plain port: the logon page disables TLS for it entirely.
                    table.getLong("tls_port", 0L).intValue(),
                    "proprietary".equalsIgnoreCase(table.getString("logon_mode", "standard"))
                            ? LogonMode.PROPRIETARY
                            : LogonMode.STANDARD));
        }
        return List.copyOf(endpoints);
    }

    private static String required(Toml table, String field) {
        String value = table.getString(field);
        if (value == null || value.isBlank()) {
            throw new IllegalStateException("[[gateway]] entry is missing '" + field + "'");
        }
        return value;
    }
}
