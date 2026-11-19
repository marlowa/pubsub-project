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
import com.pubsub.admin.service.FirstAdministratorSetup;
import com.pubsub.admin.service.LastStateFile;
import com.pubsub.admin.service.SetupSecret;
import com.pubsub.admin.service.TechnicalEventsSubscriber;
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
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import javax.sql.DataSource;
import java.io.IOException;
import java.nio.file.Paths;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class Main {
    private static final Logger log = LoggerFactory.getLogger(Main.class);
    /** The exit status of a component that stopped for the end of the trading day, as the C++ components use it. */
    private static final int STOPPED_FOR_THE_END_OF_DAY_STATUS = 3;

    public static void main(String[] args) {
        Config config = Config.load();
        // The trading day the service was started for, given on its command line as every component's is; 0 when absent,
        // and the service then never stops for an end of day (docs/venue/start_and_end_of_day.md, section 4.4).
        int tradingDay = tradingDayFrom(args);
        if (tradingDay != 0 && LastStateFile.says(config.lastStateFile(), tradingDay, LastStateFile.END_OF_DAY_END)) {
            log.info("{} says the end of trading day {} has been reached -- nothing to do, exiting for the end of day",
                    config.lastStateFile(), tradingDay);
            System.exit(STOPPED_FOR_THE_END_OF_DAY_STATUS);
        }
        DataSource dataSource = Database.createDataSource(config);

        FirmDao firmDao = new FirmDao(dataSource, config.tablePrefix());
        CompIdDao compIdDao = new CompIdDao(dataSource, config.tablePrefix());
        GatewayPermissionDao gatewayPermissionDao =
                new GatewayPermissionDao(dataSource, config.tablePrefix());

        AuthServiceClient authServiceClient = null;
        if (config.authServiceEnabled()) {
            // Fan credential updates out to both auth instances (active/active) so
            // each keeps its in-memory credential cache current; the secondary is
            // optional (omitted when its host is blank).
            List<AuthServiceClient.Endpoint> authEndpoints = new ArrayList<>();
            authEndpoints.add(new AuthServiceClient.Endpoint(
                    config.authServiceHost(), config.authServiceAdminPort()));
            if (config.authServiceSecondaryHost() != null
                    && !config.authServiceSecondaryHost().isBlank()) {
                authEndpoints.add(new AuthServiceClient.Endpoint(
                        config.authServiceSecondaryHost(), config.authServiceSecondaryAdminPort()));
            }
            authServiceClient = new AuthServiceClient(authEndpoints);
        }

        AdminUserStore adminUserStore = new AdminUserStore(Paths.get(config.adminUsersFile()));
        // The one-time secret first-run setup asks for is kept beside the administrators' file (R-0153).
        FirstAdministratorSetup firstAdministratorSetup = new FirstAdministratorSetup(adminUserStore,
                new SetupSecret(Paths.get(config.adminUsersFile()).toAbsolutePath().resolveSibling("admin_setup_secret.txt")));
        if (firstAdministratorSetup.prepare()) {
            log.warn("No administrator exists. First-run setup at /setup asks for the one-time secret in {}, which only the account "
                    + "running this editor can read; creating the first administrator deletes it",
                    firstAdministratorSetup.secretFile());
        }

        FirmHandler firmHandler = new FirmHandler(firmDao, compIdDao, authServiceClient);
        CompIdHandler compIdHandler = new CompIdHandler(compIdDao, firmDao, authServiceClient);
        GatewayPermissionHandler gwHandler =
                new GatewayPermissionHandler(gatewayPermissionDao, compIdDao);
        AuthFilter authFilter = new AuthFilter(adminUserStore);
        LoginHandler loginHandler = new LoginHandler(adminUserStore);
        SetupHandler setupHandler = new SetupHandler(firstAdministratorSetup);
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
        app.exception(Exception.class, (e, ctx) -> {
                log.error("Unhandled exception for {} {}", ctx.req().getMethod(), ctx.req().getRequestURI(), e);
                ctx.status(500).render("/templates/error.ftl",
                        Map.of("message", "Internal error: " + e.getMessage()));
        });

        app.start(config.serverPort());

        // The end of the trading day arrives on the technical_events topic; the service then records it and exits for
        // the end of day (decision 11).
        if (tradingDay != 0) {
            List<TechnicalEventsSubscriber.Endpoint> publishers = new ArrayList<>();
            if (!config.technicalEventsPrimaryHost().isBlank()) {
                publishers.add(new TechnicalEventsSubscriber.Endpoint(config.technicalEventsPrimaryHost(), config.technicalEventsPrimaryPort()));
            }
            if (!config.technicalEventsSecondaryHost().isBlank()) {
                publishers.add(new TechnicalEventsSubscriber.Endpoint(config.technicalEventsSecondaryHost(), config.technicalEventsSecondaryPort()));
            }
            new TechnicalEventsSubscriber(publishers, tradingDay, "admin_service", Duration.ofSeconds(1), (day, seqNo) -> {
                log.info("the end-of-day end of trading day {} at seq={} arrived on the technical_events topic -- stopping for the end of day",
                        day, seqNo);
                try {
                    LastStateFile.record(config.lastStateFile(), day, LastStateFile.END_OF_DAY_END);
                } catch (IOException e) {
                    log.error("could not record the end of the day in {}: {}", config.lastStateFile(), e.getMessage());
                }
                app.stop();
                System.exit(STOPPED_FOR_THE_END_OF_DAY_STATUS);
            }).start();
        }
    }

    /** The value of {@code --trading-day YYYYMMDD} among {@code args}, or 0 when it is not given. */
    static int tradingDayFrom(String[] args) {
        for (int index = 0; index + 1 < args.length; ++index) {
            if ("--trading-day".equals(args[index])) {
                return Integer.parseInt(args[index + 1]);
            }
        }
        return 0;
    }
}
