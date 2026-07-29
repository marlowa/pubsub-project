# PubSub Admin Service

A web-based administration UI for managing firms, comp IDs, gateway permissions,
and SCRAM-SHA-256 credentials for the PubSub system.

Built with Javalin 6, Freemarker 2.3, and plain JDBC (HikariCP). No CSS
framework — the UI is styled by `src/main/resources/static/desktop.css`.

## Prerequisites

- Java 17+
- Maven 3.8+
- PostgreSQL (schema already created by `db/create_db.py` + Liquibase)
- PubSub authentication service running on its TLS admin port (default 7072)

## Building

```
mvn package
```

Produces `target/admin-service-0.0.1-SNAPSHOT.jar` — a self-contained fat JAR.

## Running

```
java -jar target/admin-service-0.0.1-SNAPSHOT.jar
```

Configuration is read from `application.properties` on the classpath (bundled in the
JAR). To override individual properties without rebuilding, use system properties:

```
java -Dserver.port=9090 -jar target/admin-service-0.0.1-SNAPSHOT.jar
```

The service starts on port 8080 by default.

## First-Run Setup

On the first visit, the service detects that no admin users exist and redirects to
`/setup`. Enter a username and password (minimum 8 characters) to create the initial
ADMIN account, then sign in at `/login`.

Admin user credentials are stored in `admin_users.toml` (path configurable via
`admin.users-file`). This file is created automatically on first use. It does not
require a database connection — the admin UI can be accessed even if PostgreSQL is
temporarily unavailable.

## User Roles

| Role   | Capability |
|--------|-----------|
| ADMIN  | Full access: create/edit/delete firms, comp IDs, gateway permissions, and admin users |
| VIEWER | Read-only: all GET pages; POST routes are blocked with HTTP 403 |

Accounts created by an ADMIN have **force-password-change** set. The new user is
redirected to `/change-password` immediately after their first login.

## Credential Lifecycle

The admin service keeps credentials in two places in sync: the PostgreSQL database
(SCRAM-derived values only — no plaintext password is ever stored) and the running
authentication service (updated live via PDU 510/512/514 over the TLS admin channel).

### What happens automatically

| Action | Effect on auth service |
|--------|----------------------|
| Set password on comp_id | PDU 510 sent — credential activated |
| Disable firm | PDU 512 sent for every comp_id in the firm — credentials revoked |
| Delete firm | PDU 512 sent for every comp_id in the firm — credentials revoked |
| Disable comp_id | PDU 512 sent — credential revoked |
| Lock comp_id | PDU 512 sent — credential revoked |
| Delete comp_id | PDU 512 sent — credential revoked |
| Re-enable firm | PDU 514 sent for every enabled+unlocked comp_id — credentials restored |
| Re-enable comp_id | PDU 514 sent — credential restored |
| Unlock comp_id | PDU 514 sent — credential restored |

PDU 514 (RestoreCredentialRequest) carries the pre-derived SCRAM fields directly from
the database — no plaintext password is needed or transmitted. The authentication service
installs the credential into its in-memory store and persists it to credentials.toml.

## Configuration Reference

All properties live in `application.properties`. Defaults shown.

```properties
# Database
db.url=jdbc:postgresql://localhost:5432/pubsub
db.username=pubsub_app
db.password=pubsub_dev
db.table-prefix=pubsub_

# Authentication service (TLS admin channel)
# Set to false during DB-only development when the auth service is not running.
auth-service.enabled=true
auth-service.host=127.0.0.1
auth-service.admin-port=7072

# HTTP server
server.port=8080

# Admin UI user store (created automatically)
admin.users-file=admin_users.toml

# Branding (see Branding section below)
brand.name=PubSub Admin
brand.logo-url=
brand.css-file=
```

Environment variable overrides are available for secrets:

| Environment variable       | Property overridden |
|----------------------------|---------------------|
| `PUBSUB_DB_URL`            | `db.url`            |
| `PUBSUB_DB_USERNAME`       | `db.username`       |
| `PUBSUB_APP_DB_PASSWORD`   | `db.password`       |

## Branding

Three properties control the look of the UI. All are optional — the service runs
fine with the defaults.

### `brand.name`

The product name. Appears in every page title (`<title>`) and in the navigation
header. Also used as the heading on the login and first-run setup pages.

```properties
brand.name=Acme Messaging Admin
```

### `brand.logo-url`

URL of a logo image. If set, the image appears:

- In the navigation header (alongside `brand.name`), constrained to 1.5 rem height
- Centred above the heading on the login and first-run setup pages, constrained to
  3 rem height

The URL can be anything the browser can resolve:

```properties
# Intranet URL
brand.logo-url=http://intranet.acme.com/assets/logo.png

# File served from the JAR's own /static/ path
brand.logo-url=/static/logo.png
```

If you use `/static/logo.png`, place the file at
`src/main/resources/static/logo.png` and rebuild. For a no-rebuild option, use an
intranet URL.

### `brand.css-file`

Filesystem path to a CSS file. Its content is inlined as a `<style>` block into
every page — including the login and setup pages which have no nav bar. The file
is read once at startup; the service must be restarted to pick up changes.

Use this to apply your organisation's colour palette without modifying or
rebuilding the JAR. The stylesheet declares its brandable colours as custom
properties in a `:root` block, and `brand.css` is inlined *after* the stylesheet
link, so redeclaring any of them wins.

```properties
brand.css-file=/etc/pubsub-admin/brand.css
```

**Example `brand.css`:**

```css
:root {
    /* Links and other accents */
    --admin-accent-colour:       #003087;
    --admin-accent-hover-colour: #00205b;

    /* The menu bar across the top of every page */
    --admin-chrome-background:    #dfe4ec;
    --admin-chrome-border-colour: #9aa4b4;

    /* Page background and body text */
    --admin-page-background: #f4f4f4;
    --admin-text-colour:     #111;
}
```

Those six properties are the whole supported branding surface, and the
authoritative list is the `:root` block at the top of
`src/main/resources/static/desktop.css`. The rest of the stylesheet — the
monospace type, the dense tables, the bevelled buttons — is fixed on purpose:
this is a desktop administration tool, and the density is the design rather than
a preference. You can of course override any other rule from `brand.css`, but
those selectors are not a stable interface and may change.

The UI uses no CSS framework. `desktop.css` is served from the JAR at
`/static/desktop.css`, so the service has no outbound network dependency and
works in air-gapped environments.

### Worked example — full branding for "Acme Messaging"

1. Put your logo at `/etc/pubsub-admin/logo.png`.
2. Create `/etc/pubsub-admin/brand.css` with your colour overrides.
3. In `application.properties`:

```properties
brand.name=Acme Messaging Admin
brand.logo-url=/static/logo.png
brand.css-file=/etc/pubsub-admin/brand.css
```

4. Because `brand.logo-url=/static/logo.png` references the built-in static path,
   place `logo.png` at `src/main/resources/static/logo.png` and rebuild. Alternatively,
   change `brand.logo-url` to an intranet URL and skip the rebuild.

5. Restart the service.
