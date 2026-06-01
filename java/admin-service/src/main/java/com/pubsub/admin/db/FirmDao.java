package com.pubsub.admin.db;

import com.pubsub.admin.model.FirmRow;

import javax.sql.DataSource;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.OffsetDateTime;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public class FirmDao {
    private final DataSource dataSource;
    private final String table;

    public FirmDao(DataSource dataSource, String tablePrefix) {
        this.dataSource = dataSource;
        this.table = tablePrefix + "firm";
    }

    public List<FirmRow> listAll() throws SQLException {
        List<FirmRow> rows = new ArrayList<>();
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "SELECT firm_id, name, enabled, created_at, updated_at"
                     + " FROM " + table + " ORDER BY firm_id");
             ResultSet rs = ps.executeQuery()) {
            while (rs.next()) {
                rows.add(mapRow(rs));
            }
        }
        return rows;
    }

    public Optional<FirmRow> findById(String firmId) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "SELECT firm_id, name, enabled, created_at, updated_at"
                     + " FROM " + table + " WHERE firm_id = ?")) {
            ps.setString(1, firmId);
            try (ResultSet rs = ps.executeQuery()) {
                return rs.next() ? Optional.of(mapRow(rs)) : Optional.empty();
            }
        }
    }

    public void insert(String firmId, String name) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "INSERT INTO " + table + " (firm_id, name) VALUES (?, ?)")) {
            ps.setString(1, firmId);
            ps.setString(2, name);
            ps.executeUpdate();
        }
    }

    public void update(String firmId, String name, boolean enabled) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "UPDATE " + table
                     + " SET name = ?, enabled = ?, updated_at = NOW()"
                     + " WHERE firm_id = ?")) {
            ps.setString(1, name);
            ps.setBoolean(2, enabled);
            ps.setString(3, firmId);
            ps.executeUpdate();
        }
    }

    public void delete(String firmId) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "DELETE FROM " + table + " WHERE firm_id = ?")) {
            ps.setString(1, firmId);
            ps.executeUpdate();
        }
    }

    private FirmRow mapRow(ResultSet rs) throws SQLException {
        return new FirmRow(
                rs.getString("firm_id"),
                rs.getString("name"),
                rs.getBoolean("enabled"),
                rs.getObject("created_at", OffsetDateTime.class),
                rs.getObject("updated_at", OffsetDateTime.class));
    }
}
