package com.pubsub.admin.db;

import com.pubsub.admin.model.GatewayPermissionRow;

import javax.sql.DataSource;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.OffsetDateTime;
import java.util.ArrayList;
import java.util.List;

public class GatewayPermissionDao {
    private final DataSource dataSource;
    private final String table;

    public GatewayPermissionDao(DataSource dataSource, String tablePrefix) {
        this.dataSource = dataSource;
        this.table = tablePrefix + "comp_id_gateway_permission";
    }

    public List<GatewayPermissionRow> listByCompId(String compId) throws SQLException {
        List<GatewayPermissionRow> rows = new ArrayList<>();
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "SELECT comp_id, gateway_type, enabled, created_at FROM " + table
                     + " WHERE comp_id = ? ORDER BY gateway_type")) {
            ps.setString(1, compId);
            try (ResultSet rs = ps.executeQuery()) {
                while (rs.next()) {
                    rows.add(mapRow(rs));
                }
            }
        }
        return rows;
    }

    public boolean exists(String compId, String gatewayType) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "SELECT 1 FROM " + table + " WHERE comp_id = ? AND gateway_type = ?")) {
            ps.setString(1, compId);
            ps.setString(2, gatewayType);
            try (ResultSet rs = ps.executeQuery()) {
                return rs.next();
            }
        }
    }

    public void insert(String compId, String gatewayType) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "INSERT INTO " + table + " (comp_id, gateway_type) VALUES (?, ?)")) {
            ps.setString(1, compId);
            ps.setString(2, gatewayType);
            ps.executeUpdate();
        }
    }

    public void delete(String compId, String gatewayType) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "DELETE FROM " + table + " WHERE comp_id = ? AND gateway_type = ?")) {
            ps.setString(1, compId);
            ps.setString(2, gatewayType);
            ps.executeUpdate();
        }
    }

    private GatewayPermissionRow mapRow(ResultSet rs) throws SQLException {
        return new GatewayPermissionRow(
                rs.getString("comp_id"),
                rs.getString("gateway_type"),
                rs.getBoolean("enabled"),
                rs.getObject("created_at", OffsetDateTime.class));
    }
}
