package com.pubsub.fixtestclient;

import com.moandjiezana.toml.Toml;

import java.io.File;

public record Config(
        int serverPort,
        String outputDir,
        String scriptsDir,
        String gatewayHost,
        int gatewayPort,
        String targetCompId,
        String trustStorePath,
        String trustStorePassword
) {
    public static Config load(String path) {
        Toml toml = new Toml().read(new File(path));
        return new Config(
                toml.getLong("server.port", 8081L).intValue(),
                toml.getString("capture.output_dir", "output"),
                toml.getString("scripts.scripts_dir", "scripts"),
                toml.getString("fix.gateway_host", "127.0.0.1"),
                toml.getLong("fix.gateway_port", 9879L).intValue(),
                toml.getString("fix.target_comp_id", "GATEWAY"),
                toml.getString("fix.trust_store_path", "config/fix_gateway_trust.jks"),
                toml.getString("fix.trust_store_password", "pubsub_dev")
        );
    }
}
