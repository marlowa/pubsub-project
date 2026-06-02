package com.pubsub.fixtestclient;

import com.pubsub.fixtestclient.blotter.BlotterStore;
import com.pubsub.fixtestclient.capture.MessageCapture;
import com.pubsub.fixtestclient.fix.FixApplication;
import com.pubsub.fixtestclient.fix.FixEngine;
import com.pubsub.fixtestclient.log.LogBuffer;
import com.pubsub.fixtestclient.script.FixHelper;
import com.pubsub.fixtestclient.script.FixSessionBinding;
import com.pubsub.fixtestclient.script.ScriptRunner;
import com.pubsub.fixtestclient.web.ConfigHandler;
import com.pubsub.fixtestclient.web.LogHandler;
import com.pubsub.fixtestclient.web.MessagesHandler;
import com.pubsub.fixtestclient.web.ScriptHandler;
import com.pubsub.fixtestclient.web.SessionHandler;
import io.javalin.Javalin;
import io.javalin.http.staticfiles.Location;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class Main {

    private static final Logger log = LoggerFactory.getLogger(Main.class);

    public static void main(String[] args) throws Exception {
        String configPath = args.length > 0 ? args[0] : "config/app.toml";
        Config config = Config.load(configPath);

        LogBuffer logBuffer = LogBuffer.getInstance();
        logBuffer.start();

        FixApplication fixApplication = new FixApplication();
        FixEngine fixEngine = new FixEngine(config.sessionConfig(), fixApplication);
        fixEngine.start();

        BlotterStore blotterStore = new BlotterStore();
        fixApplication.setInboundListener(blotterStore::addInbound);

        MessageCapture messageCapture = new MessageCapture(config.outputDir());
        fixApplication.setInboundListener(message -> {
            blotterStore.addInbound(message);
            messageCapture.capture(message);
        });

        FixSessionBinding sessionBinding = new FixSessionBinding(fixEngine);
        FixHelper fixHelper = new FixHelper();
        ScriptRunner scriptRunner = new ScriptRunner(sessionBinding, fixHelper, messageCapture);

        SessionHandler sessionHandler = new SessionHandler(fixEngine);
        ScriptHandler scriptHandler = new ScriptHandler(scriptRunner, config.scriptsDir());
        MessagesHandler messagesHandler = new MessagesHandler(fixEngine, blotterStore);
        ConfigHandler configHandler = new ConfigHandler(configPath, config.sessionConfig());
        LogHandler logHandler = new LogHandler(logBuffer);

        Javalin app = Javalin.create(cfg -> {
            cfg.staticFiles.add(conf -> {
                conf.hostedPath = "/";
                conf.directory = "/web";
                conf.location = Location.CLASSPATH;
            });
        });

        app.get("/api/session",            sessionHandler::getStatus);
        app.post("/api/session/logon",     sessionHandler::logon);
        app.post("/api/session/logout",    sessionHandler::logout);
        app.get("/api/session/last",       sessionHandler::getLastSession);

        app.get("/api/script",             scriptHandler::getState);
        app.post("/api/script/run",        scriptHandler::run);
        app.post("/api/script/stop",       scriptHandler::stop);
        app.get("/api/scripts",            scriptHandler::listScripts);
        app.get("/api/scripts/{filename}", scriptHandler::loadScript);
        app.post("/api/scripts/save",      scriptHandler::saveScript);

        app.get("/api/messages",           messagesHandler::getBlotter);
        app.post("/api/messages/send",     messagesHandler::send);

        app.get("/api/config",             configHandler::getConfig);

        app.get("/api/logs",               logHandler::getSnapshot);
        app.sse("/api/logs/stream",        logHandler::stream);

        app.exception(IllegalArgumentException.class, (e, ctx) ->
                ctx.status(400).json(java.util.Map.of("error", e.getMessage())));
        app.exception(Exception.class, (e, ctx) -> {
            log.error("Unhandled exception", e);
            ctx.status(500).json(java.util.Map.of("error", "Internal error: " + e.getMessage()));
        });

        app.start(config.serverPort());
        log.info("fix-test-client started on http://localhost:{}", config.serverPort());
    }
}
