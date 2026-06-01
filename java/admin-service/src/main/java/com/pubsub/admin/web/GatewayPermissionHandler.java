package com.pubsub.admin.web;

import com.pubsub.admin.db.CompIdDao;
import com.pubsub.admin.db.GatewayPermissionDao;
import com.pubsub.admin.exception.ConflictException;
import com.pubsub.admin.exception.NotFoundException;
import io.javalin.http.Context;

import java.sql.SQLException;
import java.util.Map;

public class GatewayPermissionHandler {
    private final GatewayPermissionDao gatewayPermissionDao;
    private final CompIdDao compIdDao;

    public GatewayPermissionHandler(GatewayPermissionDao gatewayPermissionDao, CompIdDao compIdDao) {
        this.gatewayPermissionDao = gatewayPermissionDao;
        this.compIdDao = compIdDao;
    }

    public void list(Context ctx) throws SQLException {
        String compId = ctx.pathParam("compId");
        compIdDao.findById(compId)
                .orElseThrow(() -> new NotFoundException("CompID not found: " + compId));
        ctx.render("/templates/gateway-permissions/list.ftl", Map.of(
                "compId", compId,
                "permissions", gatewayPermissionDao.listByCompId(compId)));
    }

    public void add(Context ctx) throws SQLException {
        String compId = ctx.pathParam("compId");
        String gatewayType = ctx.formParam("gatewayType");
        if (gatewayType == null || gatewayType.isBlank()) {
            throw new IllegalArgumentException("Missing required field: gatewayType");
        }
        gatewayType = gatewayType.trim();
        if (gatewayPermissionDao.exists(compId, gatewayType)) {
            throw new ConflictException(
                    "Gateway type '" + gatewayType + "' already exists for CompID '" + compId + "'");
        }
        gatewayPermissionDao.insert(compId, gatewayType);
        ctx.redirect("/comp-ids/" + compId + "/gateways");
    }

    public void delete(Context ctx) throws SQLException {
        String compId = ctx.pathParam("compId");
        String gatewayType = ctx.pathParam("gatewayType");
        gatewayPermissionDao.delete(compId, gatewayType);
        ctx.redirect("/comp-ids/" + compId + "/gateways");
    }
}
