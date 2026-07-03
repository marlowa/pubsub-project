# Admin Service

## Role

The admin service is a Java web application that manages the credential and access-control
database. It is the only component (besides `db/export_credentials.py`) that touches
PostgreSQL directly. It provides:

- Full CRUD for firms, comp_ids, and gateway permissions.
- Password management: derives SCRAM-SHA-256 credentials, writes them to the database, and
  pushes them live to the authentication service via PDU 510/512/514 over TLS.
- A read-only credential export path (`db/export_credentials.py`) that snapshots current
  credentials to `credentials.toml` for the authentication service to load at startup.

**Technology stack:** Java 17, Javalin 6, Freemarker 2.3, plain JDBC (HikariCP),
PostgreSQL, Pico.css, Maven. No Spring.

## Web UI

The UI is server-rendered HTML via Freemarker templates with Pico.css classless styling.
Pico.css is bundled in the JAR (`src/main/resources/static/`) — no CDN dependency; works
in air-gapped environments.

**Pages:**
- **Firms** — list, create, edit (name, enabled flag). Disabling a firm revokes all its
  comp_id credentials via PDU 512.
- **Comp IDs** — list per firm, create, edit (enabled, locked, force-password-change). Edit
  form shows a warning when re-enabling/unlocking requires a manual password reset.
- **Gateway Permissions** — list, inline add form.
- **Set Password** — per-comp-id page; derives SCRAM → writes DB → sends PDU 510 to the
  authentication service.

**Admin UI authentication:** Jenkins-style login system backed by `admin_users.toml` (no
database dependency). BCrypt-hashed passwords (jbcrypt 0.4, cost 12). Two roles:

| Role | Access |
|------|--------|
| `ADMIN` | Full CRUD; all POST routes |
| `VIEWER` | Read-only; POST routes blocked with 403 by `AuthFilter` |

First-run setup wizard (`/setup`) creates the initial ADMIN account when no users exist.
Force-password-change flag is set on admin-created accounts; users are redirected to
`/change-password` on next login. Session auth uses Jetty `SessionHandler`; `AuthFilter`
runs as a Javalin `before()` handler.

**Branding:** three properties in `application.properties`:

| Property | Purpose |
|----------|---------|
| `brand.name` | Product name shown in page titles and nav |
| `brand.logo-url` | Logo image in nav and login page |
| `brand.css-file` | Path to a CSS file inlined into every page for colour overrides |

## CompID Notifications

Credential lifecycle is kept in sync between the database and the live authentication
service via PDU:

| PDU | ID | Trigger |
|-----|----|---------|
| `SetCredentialRequest` | 510 | Password set — admin derives SCRAM, pushes to auth service |
| `SetCredentialResult` | 511 | Auth service confirms credential installed |
| `RemoveCredentialRequest` | 512 | Firm or comp_id disabled, locked, or deleted |
| `RemoveCredentialResult` | 513 | Auth service confirms credential removed |
| `RestoreCredentialRequest` | 514 | Comp_id re-enabled or unlocked; restores pre-derived SCRAM fields |
| `RestoreCredentialResult` | 515 | Auth service confirms credential restored |

**Credential lifecycle gap:** PDU 510 requires the plaintext password, which is never
stored after derivation. Re-enabling or unlocking a comp_id does NOT automatically restore
the auth service credential. The admin must reset the password afterwards. The Edit forms
show a warning when this applies.

PDU 514 (`RestoreCredentialRequest`) carries pre-derived binary SCRAM fields (`StoredKey`,
`ServerKey`, `salt`, `iterations`) and does not require the plaintext password. It is used
by the comp_id and firm re-enable flows to restore credentials previously revoked by
PDU 512.

## Build

```
cd java/admin-service && mvn package
java -jar target/admin-service-*.jar
```

Service listens on port 8080.

**Maven plugins:** Checkstyle, SpotBugs (with DI false-positive exclude filter), JaCoCo
(80% line coverage threshold), OWASP Dependency Check (run manually with
`mvn dependency-check:check`; not bound to the build lifecycle).

**Logging:** SLF4J API + Logback 1.2.13. `logback.xml` suppresses Javalin/Jetty/HikariCP
noise to WARN. Logback 1.5.x is incompatible with Javalin 6.3.0's SLF4J 1.x dependency;
1.2.13 is the correct version.

## Configuration

| File | Purpose |
|------|---------|
| `application.properties` | DB URL, JDBC credentials, auth service endpoints, branding |
| `admin_users.toml` | Admin UI user accounts (BCrypt passwords, roles) — gitignored |

See `java/admin-service/README.md` for full deployment and branding instructions.

## See Also

- [Secure Communications](../design/secure_comms.md) — SCRAM-SHA-256 derivation, auth service PDU protocol
