package com.pubsub.admin;

import com.pubsub.admin.db.CompIdDao;
import com.pubsub.admin.db.Database;
import com.pubsub.admin.db.FirmDao;
import com.pubsub.admin.db.GatewayPermissionDao;
import com.pubsub.admin.exception.ConflictException;
import com.pubsub.admin.exception.ForbiddenException;
import com.pubsub.admin.exception.NotFoundException;
import com.pubsub.admin.service.AdminUserStore;
import com.pubsub.admin.service.AuthServiceClient;
import com.pubsub.admin.web.AuthFilter;
import com.pubsub.admin.web.ChangePasswordHandler;
import com.pubsub.admin.web.CompIdHandler;
import com.pubsub.admin.web.FirmHandler;
import com.pubsub.admin.web.FreemarkerRenderer;
import com.pubsub.admin.web.GatewayPermissionHandler;
import com.pubsub.admin.web.LoginHandler;
import com.pubsub.admin.web.SetupHandler;
import com.pubsub.admin.web.UserManagementHandler;
import io.javalin.Javalin;
import io.javalin.http.staticfiles.Location;
import org.eclipse.jetty.server.session.SessionHandler;

import javax.sql.DataSource;
import java.nio.file.Paths;
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

        AdminUserStore adminUserStore = new AdminUserStore(Paths.get(config.adminUsersFile()));

        FirmHandler firmHandler = new FirmHandler(firmDao, compIdDao, authServiceClient);
        CompIdHandler compIdHandler = new CompIdHandler(compIdDao, firmDao, authServiceClient);
        GatewayPermissionHandler gwHandler =
                new GatewayPermissionHandler(gatewayPermissionDao, compIdDao);
        AuthFilter authFilter = new AuthFilter(adminUserStore);
        LoginHandler loginHandler = new LoginHandler(adminUserStore);
        SetupHandler setupHandler = new SetupHandler(adminUserStore);
        ChangePasswordHandler changePasswordHandler = new ChangePasswordHandler(adminUserStore);
        UserManagementHandler userMgmtHandler = new UserManagementHandler(adminUserStore);

        Javalin app = Javalin.create(cfg -> {
            cfg.fileRenderer(new FreemarkerRenderer(
                    config.brandName(), config.brandLogoUrl(), config.brandCssFile()));
            cfg.staticFiles.add(conf -> {
                conf.hostedPath = "/static";
                conf.directory  = "/static";
                conf.location   = Location.CLASSPATH;
            });
            cfg.jetty.modifyServletContextHandler(h -> h.setSessionHandler(new SessionHandler()));
        });

        app.before(authFilter::apply);

        app.get("/login",           loginHandler::showLogin);
        app.post("/login",          loginHandler::login);
        app.post("/logout",         loginHandler::logout);
        app.get("/setup",           setupHandler::show);
        app.post("/setup",          setupHandler::create);
        app.get("/change-password",  changePasswordHandler::show);
        app.post("/change-password", changePasswordHandler::change);

        app.get("/admin/users",                                  userMgmtHandler::list);
        app.get("/admin/users/new",                              userMgmtHandler::showNew);
        app.post("/admin/users",                                 userMgmtHandler::create);
        app.get("/admin/users/{username}/edit",                  userMgmtHandler::showEdit);
        app.post("/admin/users/{username}",                      userMgmtHandler::update);
        app.get("/admin/users/{username}/reset-password",        userMgmtHandler::showResetPassword);
        app.post("/admin/users/{username}/reset-password",       userMgmtHandler::resetPassword);
        app.post("/admin/users/{username}/delete",               userMgmtHandler::delete);

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

        app.exception(ForbiddenException.class, (e, ctx) ->
                ctx.status(403).render("/templates/error.ftl",
                        Map.of("message", e.getMessage())));
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
