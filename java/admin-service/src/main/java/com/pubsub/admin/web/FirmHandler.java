package com.pubsub.admin.web;

import com.pubsub.admin.db.CompIdDao;
import com.pubsub.admin.db.FirmDao;
import com.pubsub.admin.exception.ConflictException;
import com.pubsub.admin.exception.NotFoundException;
import com.pubsub.admin.model.CompIdRow;
import com.pubsub.admin.model.FirmRow;
import com.pubsub.admin.service.AuthServiceClient;
import io.javalin.http.Context;

import java.io.IOException;
import java.sql.SQLException;
import java.util.List;
import java.util.Map;

public class FirmHandler {
    private final FirmDao firmDao;
    private final CompIdDao compIdDao;
    private final AuthServiceClient authServiceClient;

    public FirmHandler(FirmDao firmDao, CompIdDao compIdDao, AuthServiceClient authServiceClient) {
        this.firmDao = firmDao;
        this.compIdDao = compIdDao;
        this.authServiceClient = authServiceClient;
    }

    public void list(Context ctx) throws SQLException {
        ctx.render("/templates/firms/list.ftl", Map.of("firms", firmDao.listAll()));
    }

    public void showNew(Context ctx) {
        ctx.render("/templates/firms/form.ftl", Map.of());
    }

    public void create(Context ctx) throws SQLException {
        String firmId = requireParam(ctx, "firmId");
        String name = requireParam(ctx, "name");
        if (firmDao.findById(firmId).isPresent()) {
            throw new ConflictException("Firm '" + firmId + "' already exists");
        }
        firmDao.insert(firmId, name);
        ctx.redirect("/firms");
    }

    public void showEdit(Context ctx) throws SQLException {
        String firmId = ctx.pathParam("firmId");
        FirmRow firm = firmDao.findById(firmId)
                .orElseThrow(() -> new NotFoundException("Firm not found: " + firmId));
        ctx.render("/templates/firms/form.ftl", Map.of("firm", firm));
    }

    public void update(Context ctx) throws SQLException, IOException {
        String firmId = ctx.pathParam("firmId");
        if (firmDao.findById(firmId).isEmpty()) {
            throw new NotFoundException("Firm not found: " + firmId);
        }
        String name = requireParam(ctx, "name");
        boolean enabled = "on".equals(ctx.formParam("enabled"));
        firmDao.update(firmId, name, enabled);
        if (!enabled && authServiceClient != null) {
            removeAllCompIdCredentials(firmId);
        }
        ctx.redirect("/firms");
    }

    public void delete(Context ctx) throws SQLException, IOException {
        String firmId = ctx.pathParam("firmId");
        if (authServiceClient != null) {
            removeAllCompIdCredentials(firmId);
        }
        firmDao.delete(firmId);
        ctx.redirect("/firms");
    }

    private void removeAllCompIdCredentials(String firmId) throws SQLException, IOException {
        List<CompIdRow> compIds = compIdDao.listByFirm(firmId);
        for (CompIdRow row : compIds) {
            authServiceClient.removeCredential(row.compId());
        }
    }

    private static String requireParam(Context ctx, String name) {
        String value = ctx.formParam(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("Missing required field: " + name);
        }
        return value.trim();
    }
}
