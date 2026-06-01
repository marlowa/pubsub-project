package com.pubsub.admin;

import com.pubsub.admin.db.CompIdDao;
import com.pubsub.admin.db.Database;
import com.pubsub.admin.db.FirmDao;
import com.pubsub.admin.db.GatewayPermissionDao;
import com.pubsub.admin.exception.ConflictException;
import com.pubsub.admin.exception.NotFoundException;
import com.pubsub.admin.service.AuthServiceClient;
import com.pubsub.admin.web.CompIdHandler;
import com.pubsub.admin.web.FirmHandler;
import com.pubsub.admin.web.FreemarkerRenderer;
import com.pubsub.admin.web.GatewayPermissionHandler;
import io.javalin.Javalin;

import javax.sql.DataSource;
import java.util.Map;

public class Main {
    public static void main(String[] args) {
        Config config = Config.load();
        DataSource dataSource = Database.createDataSource(config);

        FirmDao firmDao = new FirmDao(dataSource, config.tablePrefix());
        CompIdDao compIdDao = new CompIdDao(dataSource, config.tablePrefix());
        GatewayPermissionDao gatewayPermissionDao =
                new GatewayPermissionDao(dataSource, config.tablePrefix());

        AuthServiceClient authServiceClient = config.authServiceEnabled()
                ? new AuthServiceClient(config.authServiceHost(), config.authServiceAdminPort())
                : null;

        FirmHandler firmHandler = new FirmHandler(firmDao, compIdDao, authServiceClient);
        CompIdHandler compIdHandler = new CompIdHandler(compIdDao, firmDao, authServiceClient);
        GatewayPermissionHandler gwHandler =
                new GatewayPermissionHandler(gatewayPermissionDao, compIdDao);

        Javalin app = Javalin.create(cfg -> cfg.fileRenderer(new FreemarkerRenderer()));

        app.get("/", ctx -> ctx.redirect("/firms"));

        app.get("/firms",              firmHandler::list);
        app.get("/firms/new",          firmHandler::showNew);
        app.post("/firms",             firmHandler::create);
        app.get("/firms/{firmId}",     firmHandler::showEdit);
        app.post("/firms/{firmId}",    firmHandler::update);
        app.post("/firms/{firmId}/delete", firmHandler::delete);

        app.get("/comp-ids",                    compIdHandler::list);
        app.get("/firms/{firmId}/comp-ids/new", compIdHandler::showNew);
        app.post("/firms/{firmId}/comp-ids",    compIdHandler::create);
        app.get("/comp-ids/{compId}",           compIdHandler::showEdit);
        app.post("/comp-ids/{compId}",          compIdHandler::update);
        app.post("/comp-ids/{compId}/delete",   compIdHandler::delete);
        app.get("/comp-ids/{compId}/password",  compIdHandler::showSetPassword);
        app.post("/comp-ids/{compId}/password", compIdHandler::setPassword);

        app.get("/comp-ids/{compId}/gateways",                       gwHandler::list);
        app.post("/comp-ids/{compId}/gateways",                      gwHandler::add);
        app.post("/comp-ids/{compId}/gateways/{gatewayType}/delete", gwHandler::delete);

        app.exception(NotFoundException.class, (e, ctx) ->
                ctx.status(404).render("/templates/error.ftl",
                        Map.of("message", e.getMessage())));
        app.exception(ConflictException.class, (e, ctx) ->
                ctx.status(409).render("/templates/error.ftl",
                        Map.of("message", e.getMessage())));
        app.exception(IllegalArgumentException.class, (e, ctx) ->
                ctx.status(400).render("/templates/error.ftl",
                        Map.of("message", e.getMessage())));
        app.exception(Exception.class, (e, ctx) ->
                ctx.status(500).render("/templates/error.ftl",
                        Map.of("message", "Internal error: " + e.getMessage())));

        app.start(config.serverPort());
    }
}
