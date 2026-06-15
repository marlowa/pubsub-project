package com.pubsub.fixtestclient.web;

import io.javalin.http.Context;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Map;

public class ConfigHandler {

    private final String appTomlPath;

    public ConfigHandler(String appTomlPath) {
        this.appTomlPath = appTomlPath;
    }

    public void getConfig(Context ctx) {
        ctx.json(Map.of(
                "appToml", readFile(appTomlPath)
        ));
    }

    private String readFile(String path) {
        try {
            return Files.readString(Paths.get(path));
        } catch (IOException e) {
            return "(could not read " + path + ": " + e.getMessage() + ")";
        }
    }
}
