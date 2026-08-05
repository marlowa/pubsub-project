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
            + " password_changed_at, created_at, updated_at,"
            + " cancel_on_disconnect_enabled, cancel_on_disconnect_grace_period_seconds,"
            + " primary_gateway_instance, backup_gateway_instance";

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

    /**
     * Sets this comp id's cancel-on-disconnect policy.
     *
     * @param gracePeriodSeconds seconds to hold a dropped session's orders, or null to
     *                           defer to the gateway's own configured default. Null and
     *                           zero are different: zero cancels immediately.
     */
    public void updateCancelOnDisconnect(String compId, boolean enabled, Integer gracePeriodSeconds) throws SQLException {
        if (gracePeriodSeconds != null && gracePeriodSeconds < 0) {
            throw new IllegalArgumentException("cancel-on-disconnect grace period must not be negative, got " + gracePeriodSeconds);
        }
        String sql = "UPDATE " + table
                + " SET cancel_on_disconnect_enabled = ?,"
                + " cancel_on_disconnect_grace_period_seconds = ?,"
                + " updated_at = NOW() WHERE comp_id = ?";
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(sql)) {
            ps.setBoolean(1, enabled);
            if (gracePeriodSeconds == null) {
                ps.setNull(2, java.sql.Types.INTEGER);
            } else {
                ps.setInt(2, gracePeriodSeconds);
            }
            ps.setString(3, compId);
            ps.executeUpdate();
        }
    }

    /**
     * Sets the gateway instances this comp id's session may log on to.
     *
     * <p>Both may be null, meaning this member is not pinned and may log on to any
     * instance. A primary with a null backup pins it to exactly one. A backup with no
     * primary is rejected by a database constraint: it would pin the member to a single
     * instance through an unrelated column being empty, which is not a thing anyone means
     * to provision.
     *
     * @param primaryInstance the instance the member is expected to use, or null for unpinned
     * @param backupInstance  the instance it may fall back to, or null for none
     */
    public void updateGatewayPinning(String compId, Integer primaryInstance, Integer backupInstance) throws SQLException {
        if (primaryInstance != null && primaryInstance < 1) {
            throw new IllegalArgumentException("primary gateway instance must be 1 or greater, got " + primaryInstance);
        }
        if (backupInstance != null && backupInstance < 1) {
            throw new IllegalArgumentException("backup gateway instance must be 1 or greater, got " + backupInstance);
        }
        if (backupInstance != null && primaryInstance == null) {
            throw new IllegalArgumentException("a backup gateway instance needs a primary; pin to one instance by setting the primary alone");
        }
        if (backupInstance != null && backupInstance.equals(primaryInstance)) {
            throw new IllegalArgumentException("backup gateway instance must differ from the primary, both were " + primaryInstance);
        }
        String sql = "UPDATE " + table
                + " SET primary_gateway_instance = ?, backup_gateway_instance = ?,"
                + " updated_at = NOW() WHERE comp_id = ?";
        try (Connection conn = dataSource.getConnection();
             PreparedStatement ps = conn.prepareStatement(sql)) {
            setNullableInt(ps, 1, primaryInstance);
            setNullableInt(ps, 2, backupInstance);
            ps.setString(3, compId);
            ps.executeUpdate();
        }
    }

    private static void setNullableInt(PreparedStatement ps, int index, Integer value) throws SQLException {
        if (value == null) {
            ps.setNull(index, java.sql.Types.SMALLINT);
        } else {
            ps.setInt(index, value);
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
                rs.getObject("updated_at", OffsetDateTime.class),
                rs.getBoolean("cancel_on_disconnect_enabled"),
                // getObject, not getInt: getInt maps SQL NULL to 0, which here means
                // "cancel immediately" rather than "use the gateway default". The two must
                // stay distinguishable.
                rs.getObject("cancel_on_disconnect_grace_period_seconds", Integer.class),
                // Boxed for the same reason: null is "not pinned", and getInt would render
                // that as instance 0, which is not a gateway instance at all.
                rs.getObject("primary_gateway_instance", Integer.class),
                rs.getObject("backup_gateway_instance", Integer.class));
    }
}
