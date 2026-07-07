package com.pubsub.fixtestclient;

import com.moandjiezana.toml.Toml;

import java.io.File;

public record Config(
        int serverPort,
        String outputDir,
        String scriptsDir,
        String gatewayHost,
        int gatewayPort,
        int tlsGatewayPort,
        int proprietaryGatewayPort,
        boolean tlsEnabled,
        String targetCompId,
        String trustStorePath,
        String trustStorePassword,
        String logoPath
) {
    public static Config load(String path) {
        Toml toml = new Toml().read(new File(path));
        int gatewayPort = toml.getLong("fix.gateway_port", 9879L).intValue();
        return new Config(
                toml.getLong("server.port", 8081L).intValue(),
                toml.getString("capture.output_dir", "output"),
                toml.getString("scripts.scripts_dir", "scripts"),
                toml.getString("fix.gateway_host", "127.0.0.1"),
                gatewayPort,
                toml.getLong("fix.tls_gateway_port", (long) gatewayPort).intValue(),
                toml.getLong("fix.proprietary_gateway_port", 30994L).intValue(),
                toml.getBoolean("fix.tls_enabled", false),
                toml.getString("fix.target_comp_id", "GATEWAY"),
                toml.getString("fix.trust_store_path", "config/fix_gateway_trust.jks"),
                toml.getString("fix.trust_store_password", "pubsub_dev"),
                toml.getString("web.logo_path", "")
        );
    }
}
