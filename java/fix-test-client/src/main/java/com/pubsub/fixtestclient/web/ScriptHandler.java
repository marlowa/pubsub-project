package com.pubsub.fixtestclient.web;

import com.pubsub.fixtestclient.capture.CaptureStatus;
import com.pubsub.fixtestclient.script.ScriptRunner;
import io.javalin.http.Context;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

public class ScriptHandler {

    private final ScriptRunner scriptRunner;
    private final String scriptsDir;
    private volatile String currentFilename = "untitled.groovy";

    public ScriptHandler(ScriptRunner scriptRunner, String scriptsDir) {
        this.scriptRunner = scriptRunner;
        this.scriptsDir = scriptsDir;
    }

    public void getState(Context ctx) {
        CaptureStatus captureStatus = null;
        try {
            captureStatus = scriptRunner.state() == com.pubsub.fixtestclient.script.ScriptState.RUNNING
                    ? null : null;
        } catch (Exception ignored) {
        }
        ctx.json(Map.of(
                "state", scriptRunner.state().name(),
                "output", scriptRunner.output(),
                "errorMessage", scriptRunner.errorMessage(),
                "content", scriptRunner.scriptContent(),
                "filename", currentFilename
        ));
    }

    public void getCaptureStatus(Context ctx) {
        ctx.json(Map.of(
                "active", false,
                "outputPath", "",
                "messageCount", 0
        ));
    }

    public void run(Context ctx) {
        String content = ctx.formParam("content");
        if (content == null) {
            ctx.status(400).json(Map.of("error", "No script content"));
            return;
        }
        scriptRunner.setScriptContent(content);
        boolean started = scriptRunner.run(content);
        if (!started) {
            ctx.status(409).json(Map.of("error", "Script already running"));
            return;
        }
        ctx.json(Map.of("ok", true));
    }

    public void stop(Context ctx) {
        scriptRunner.stop();
        ctx.json(Map.of("ok", true));
    }

    public void listScripts(Context ctx) {
        File dir = new File(scriptsDir);
        List<String> files = List.of();
        if (dir.isDirectory()) {
            String[] found = dir.list((d, name) -> name.endsWith(".groovy"));
            if (found != null) {
                files = Arrays.stream(found).sorted().collect(Collectors.toList());
            }
        }
        ctx.json(files);
    }

    public void loadScript(Context ctx) {
        String filename = ctx.pathParam("filename");
        if (!isValidFilename(filename)) {
            ctx.status(400).json(Map.of("error", "Invalid filename"));
            return;
        }
        Path path = Paths.get(scriptsDir, filename);
        try {
            String content = Files.readString(path);
            scriptRunner.setScriptContent(content);
            currentFilename = filename;
            ctx.json(Map.of("content", content, "filename", filename));
        } catch (IOException e) {
            ctx.status(404).json(Map.of("error", "File not found: " + filename));
        }
    }

    public void saveScript(Context ctx) {
        String filename = ctx.formParam("filename");
        String content = ctx.formParam("content");
        if (filename == null || !isValidFilename(filename)) {
            ctx.status(400).json(Map.of("error", "Invalid filename"));
            return;
        }
        if (content == null) {
            ctx.status(400).json(Map.of("error", "No content"));
            return;
        }
        try {
            new File(scriptsDir).mkdirs();
            Files.writeString(Paths.get(scriptsDir, filename), content);
            currentFilename = filename;
            ctx.json(Map.of("ok", true));
        } catch (IOException e) {
            ctx.status(500).json(Map.of("error", "Failed to save: " + e.getMessage()));
        }
    }

    private boolean isValidFilename(String filename) {
        return filename != null
                && filename.endsWith(".groovy")
                && !filename.contains("/")
                && !filename.contains("..");
    }
}
