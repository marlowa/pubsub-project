package com.pubsub.admin.web;

import com.pubsub.admin.db.CompIdDao;
import com.pubsub.admin.db.FirmDao;
import com.pubsub.admin.exception.ConflictException;
import com.pubsub.admin.exception.NotFoundException;
import com.pubsub.admin.model.CompIdRow;
import com.pubsub.admin.service.AuthServiceClient;
import com.pubsub.admin.service.ScramCredential;
import com.pubsub.admin.service.ScramDerivation;
import io.javalin.http.Context;

import java.io.IOException;
import java.sql.SQLException;
import java.util.HashMap;
import java.util.Map;

public class CompIdHandler {
    private static final int SCRAM_ITERATIONS = 4096;

    private final CompIdDao compIdDao;
    private final FirmDao firmDao;
    private final AuthServiceClient authServiceClient;

    public CompIdHandler(CompIdDao compIdDao, FirmDao firmDao, AuthServiceClient authServiceClient) {
        this.compIdDao = compIdDao;
        this.firmDao = firmDao;
        this.authServiceClient = authServiceClient;
    }

    public void list(Context ctx) throws SQLException {
        String firmId = ctx.queryParam("firmId");
        Map<String, Object> model = new HashMap<>();
        if (firmId != null && !firmId.isBlank()) {
            model.put("compIds", compIdDao.listByFirm(firmId));
            model.put("firmId", firmId);
        } else {
            model.put("compIds", compIdDao.listAll());
        }
        ctx.render("/templates/comp-ids/list.ftl", model);
    }

    public void showNew(Context ctx) throws SQLException {
        String firmId = ctx.pathParam("firmId");
        if (firmDao.findById(firmId).isEmpty()) {
            throw new NotFoundException("Firm not found: " + firmId);
        }
        ctx.render("/templates/comp-ids/form.ftl", Map.of("firmId", firmId));
    }

    public void create(Context ctx) throws SQLException, IOException {
        String firmId = ctx.pathParam("firmId");
        String compId = requireParam(ctx, "compId");
        String password = requireParam(ctx, "password");
        if (compIdDao.findById(compId).isPresent()) {
            throw new ConflictException("CompID '" + compId + "' already exists");
        }
        ScramCredential cred = ScramDerivation.derive(password, SCRAM_ITERATIONS);
        compIdDao.insert(compId, firmId, cred);
        if (authServiceClient != null) {
            authServiceClient.setCredential(compId, password, SCRAM_ITERATIONS);
        }
        ctx.redirect("/comp-ids?firmId=" + firmId);
    }

    public void showEdit(Context ctx) throws SQLException {
        String compId = ctx.pathParam("compId");
        CompIdRow row = compIdDao.findById(compId)
                .orElseThrow(() -> new NotFoundException("CompID not found: " + compId));
        ctx.render("/templates/comp-ids/form.ftl", Map.of("row", row));
    }

    public void update(Context ctx) throws SQLException, IOException {
        String compId = ctx.pathParam("compId");
        CompIdRow existing = compIdDao.findById(compId)
                .orElseThrow(() -> new NotFoundException("CompID not found: " + compId));
        boolean enabled = "on".equals(ctx.formParam("enabled"));
        boolean forcePasswordChange = "on".equals(ctx.formParam("forcePasswordChange"));
        boolean locked = "on".equals(ctx.formParam("locked"));
        String lockedReason = ctx.formParam("lockedReason");
        compIdDao.updateStatus(compId, enabled, forcePasswordChange, locked, lockedReason);
        if ((!enabled || locked) && authServiceClient != null) {
            authServiceClient.removeCredential(compId);
        } else if ((!existing.enabled() || existing.locked()) && enabled && !locked && authServiceClient != null) {
            ScramCredential cred = new ScramCredential(
                    existing.storedKey(), existing.serverKey(), existing.salt(), existing.iterations());
            authServiceClient.restoreCredential(compId, cred);
        }
        ctx.redirect("/comp-ids?firmId=" + existing.firmId());
    }

    public void delete(Context ctx) throws SQLException, IOException {
        String compId = ctx.pathParam("compId");
        CompIdRow existing = compIdDao.findById(compId)
                .orElseThrow(() -> new NotFoundException("CompID not found: " + compId));
        compIdDao.delete(compId);
        if (authServiceClient != null) {
            authServiceClient.removeCredential(compId);
        }
        ctx.redirect("/comp-ids?firmId=" + existing.firmId());
    }

    public void showSetPassword(Context ctx) throws SQLException {
        String compId = ctx.pathParam("compId");
        compIdDao.findById(compId)
                .orElseThrow(() -> new NotFoundException("CompID not found: " + compId));
        ctx.render("/templates/comp-ids/set-password.ftl", Map.of("compId", compId));
    }

    public void setPassword(Context ctx) throws SQLException, IOException {
        String compId = ctx.pathParam("compId");
        CompIdRow existing = compIdDao.findById(compId)
                .orElseThrow(() -> new NotFoundException("CompID not found: " + compId));
        String password = requireParam(ctx, "password");
        ScramCredential cred = ScramDerivation.derive(password, SCRAM_ITERATIONS);
        compIdDao.updateCredentials(compId, cred);
        if (authServiceClient != null) {
            authServiceClient.setCredential(compId, password, SCRAM_ITERATIONS);
        }
        ctx.redirect("/comp-ids?firmId=" + existing.firmId());
    }

    private static String requireParam(Context ctx, String name) {
        String value = ctx.formParam(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("Missing required field: " + name);
        }
        return value.trim();
    }
}
