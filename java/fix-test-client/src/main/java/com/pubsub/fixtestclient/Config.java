package com.pubsub.fixtestclient;

import com.moandjiezana.toml.Toml;

import java.io.File;

public record Config(
        int serverPort,
        String sessionConfig,
        String outputDir,
        String scriptsDir
) {
    public static Config load(String path) {
        Toml toml = new Toml().read(new File(path));
        return new Config(
                toml.getLong("server.port", 8081L).intValue(),
                toml.getString("fix.session_config", "config/session.cfg"),
                toml.getString("capture.output_dir", "output"),
                toml.getString("scripts.scripts_dir", "scripts")
        );
    }
}
