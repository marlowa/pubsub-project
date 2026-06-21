package com.pubsub.admin.db;

import com.pubsub.admin.model.CompIdRow;
import com.pubsub.admin.service.ScramCredential;

import javax.sql.DataSource;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.OffsetDateTime;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public class CompIdDao {
    private static final String SELECT_COLUMNS =
            "comp_id, firm_id, stored_key, server_key, salt, iterations,"
            + " enabled, force_password_change, consecutive_failed_logins,"
            + " locked, locked_reason, locked_at, last_login_at,"
            + " password_changed_at, created_at, updated_at";

    private final DataSource dataSource;
    private final String table;

    public CompIdDao(DataSource dataSource, String tablePrefix) {
        this.dataSource = dataSource;
        this.table = tablePrefix + "comp_id";
    }

    public List<CompIdRow> listAll() throws SQLException {
        List<CompIdRow> rows = new ArrayList<>();
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "SELECT " + SELECT_COLUMNS + " FROM " + table + " ORDER BY comp_id");
             ResultSet rs = ps.executeQuery()) {
            while (rs.next()) {
                rows.add(mapRow(rs));
            }
        }
        return rows;
    }

    public List<CompIdRow> listByFirm(String firmId) throws SQLException {
        List<CompIdRow> rows = new ArrayList<>();
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "SELECT " + SELECT_COLUMNS + " FROM " + table
                     + " WHERE firm_id = ? ORDER BY comp_id")) {
            ps.setString(1, firmId);
            try (ResultSet rs = ps.executeQuery()) {
                while (rs.next()) {
                    rows.add(mapRow(rs));
                }
            }
        }
        return rows;
    }

    public Optional<CompIdRow> findById(String compId) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "SELECT " + SELECT_COLUMNS + " FROM " + table + " WHERE comp_id = ?")) {
            ps.setString(1, compId);
            try (ResultSet rs = ps.executeQuery()) {
                return rs.next() ? Optional.of(mapRow(rs)) : Optional.empty();
            }
        }
    }

    public void insert(String compId, String firmId, ScramCredential cred,
                       boolean forcePasswordChange) throws SQLException {
        String sql = "INSERT INTO " + table
                + " (comp_id, firm_id, stored_key, server_key, salt, iterations,"
                + " force_password_change)"
                + " VALUES (?, ?, ?, ?, ?, ?, ?)";
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(sql)) {
            ps.setString(1, compId);
            ps.setString(2, firmId);
            ps.setString(3, cred.storedKey());
            ps.setString(4, cred.serverKey());
            ps.setString(5, cred.salt());
            ps.setInt(6, cred.iterations());
            ps.setBoolean(7, forcePasswordChange);
            ps.executeUpdate();
        }
    }

    public void updateStatus(String compId, boolean enabled, boolean forcePasswordChange,
                             boolean locked, String lockedReason) throws SQLException {
        String trimmedReason = (lockedReason != null && !lockedReason.isBlank())
                ? lockedReason.trim() : null;
        String sql = "UPDATE " + table
                + " SET enabled = ?, force_password_change = ?, locked = ?,"
                + " locked_reason = ?,"
                + " locked_at = CASE WHEN ? THEN NOW() ELSE NULL END,"
                + " updated_at = NOW() WHERE comp_id = ?";
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(sql)) {
            ps.setBoolean(1, enabled);
            ps.setBoolean(2, forcePasswordChange);
            ps.setBoolean(3, locked);
            ps.setString(4, trimmedReason);
            ps.setBoolean(5, locked);
            ps.setString(6, compId);
            ps.executeUpdate();
        }
    }

    public void updateCredentials(String compId, ScramCredential cred) throws SQLException {
        String sql = "UPDATE " + table
                + " SET stored_key = ?, server_key = ?, salt = ?, iterations = ?,"
                + " force_password_change = false, consecutive_failed_logins = 0,"
                + " password_changed_at = NOW(), updated_at = NOW() WHERE comp_id = ?";
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(sql)) {
            ps.setString(1, cred.storedKey());
            ps.setString(2, cred.serverKey());
            ps.setString(3, cred.salt());
            ps.setInt(4, cred.iterations());
            ps.setString(5, compId);
            ps.executeUpdate();
        }
    }

    public void delete(String compId) throws SQLException {
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(
                     "DELETE FROM " + table + " WHERE comp_id = ?")) {
            ps.setString(1, compId);
            ps.executeUpdate();
        }
    }

    private CompIdRow mapRow(ResultSet rs) throws SQLException {
        return new CompIdRow(
                rs.getString("comp_id"),
                rs.getString("firm_id"),
                rs.getString("stored_key"),
                rs.getString("server_key"),
                rs.getString("salt"),
                rs.getInt("iterations"),
                rs.getBoolean("enabled"),
                rs.getBoolean("force_password_change"),
                rs.getInt("consecutive_failed_logins"),
                rs.getBoolean("locked"),
                rs.getString("locked_reason"),
                rs.getObject("locked_at", OffsetDateTime.class),
                rs.getObject("last_login_at", OffsetDateTime.class),
                rs.getObject("password_changed_at", OffsetDateTime.class),
                rs.getObject("created_at", OffsetDateTime.class),
                rs.getObject("updated_at", OffsetDateTime.class));
    }
}
