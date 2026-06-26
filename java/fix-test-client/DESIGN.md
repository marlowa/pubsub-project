# fix-test-client — Design Document

## Purpose

A lightweight FIX 5.0 SP2 gateway test client. Single-user, single-session web
application providing session management, manual message entry, Groovy scripting
for high-volume order flows, and a log viewer. Focused on correctness and
performance, not visual sophistication.

---

## Note on SCRAM Authentication

SCRAM-SHA-256 authentication in this system is an **internal gateway concern**.
When a FIX client connects, the gateway authenticates on its behalf by performing
a SCRAM exchange with the authentication service using the client's `SenderCompID`
as the username. The FIX client itself sends only a standard FIX 5.0 SP2 Logon
message. No SCRAM logic is required in the test client.

---

## Architecture

```
Browser
  │  HTTP / SSE
  ▼
┌─────────────────────────────────────────────────────────────────┐
│  Javalin web server                                             │
│                                                                 │
│  GET /session   GET /script   GET /messages                     │
│  GET /config    GET /logs                                       │
│  POST /session/logon          POST /script/run                  │
│  POST /session/logout         GET  /logs/stream  (SSE)          │
└────────────────────────┬────────────────────────────────────────┘
                         │
          ┌──────────────┼──────────────┐
          │              │              │
          ▼              ▼              ▼
  ┌──────────────┐ ┌──────────────┐ ┌──────────────────┐
  │  FixEngine   │ │ ScriptRunner │ │   LogBuffer      │
  │              │ │              │ │                  │
  │ QuickFIX/J   │ │ GroovyShell  │ │ Logback appender │
  │ Initiator    │ │ (own thread) │ │ ring buffer      │
  │              │ │              │ │ → SSE stream     │
  └──────┬───────┘ └──────┬───────┘ └──────────────────┘
         │                │
         │         ┌──────▼──────┐
         │         │FixSession   │  script API binding
         │         │ (facade)    │
         │         └──────┬──────┘
         │                │ calls
         └────────────────┘
                  │
         ┌────────▼────────┐
         │ MessageCapture  │
         │                 │
         │ BlockingQueue   │
         │ → writer thread │
         │ → output/*.log  │
         └─────────────────┘
         │
     TCP (FIX 5.0 SP2)
         │
     Gateway :9879
```

---

## Components

### FixEngine

Wraps a QuickFIX/J `SocketInitiator`. Owns the session lifecycle.

Responsibilities:
- Build QuickFIX/J `SessionSettings` programmatically from `Config` (loaded from `app.toml`)
- Start and stop the initiator
- Expose session state (connected / logged on / sequence numbers)
- Route inbound messages to `FixApplication` callbacks
- Send outbound messages

`FixApplication` implements `quickfix.Application` and handles:
- `onCreate`, `onLogon`, `onLogout`, `onMessage` (session-level)
- `fromApp` — inbound application messages (ERs etc.)
- `toApp` — outbound application messages

QuickFIX/J handles all session-level protocol automatically:
heartbeats, test requests, resend requests, gap fill, sequence number
persistence and recovery.

### ScriptRunner

Executes Groovy scripts in a dedicated `ExecutorService` thread. The UI thread
is never blocked by script execution.

- One script runs at a time; submitting a new script while one is running is rejected
- Script state: `IDLE | RUNNING | COMPLETED | FAILED`
- Script output (stdout, errors) is captured to a `StringWriter`
- The `FixSession` facade is injected as a Groovy binding variable named `session`
- A `sleep(ms)` helper is also bound so scripts can pace themselves

### FixSession (Script API)

The object bound as `session` in Groovy scripts. Thin facade over `FixEngine`.

```
session.logon()
session.logout()
session.disconnect()
session.setNextOutgoingSeqNum(int n)
session.isLoggedOn() → boolean
session.send(quickfix.Message msg)

fix.newOrderSingle() → quickfix.fix50sp2.NewOrderSingle
fix.field(int tag, String value) → quickfix.StringField
```

`fix` is a second binding providing factory helpers so scripts do not need to
import QuickFIX/J classes by hand.

### MessageCapture

Active only while a script is running (or when explicitly enabled via the UI).

- `FixApplication.fromApp` enqueues inbound messages to a `LinkedBlockingQueue`
- A writer thread drains the queue to `output/<timestamp>.log`
- Format: one tag per line, `Tag (nnn): value`, blank line between messages
- The UI shows whether capture is active and the path of the current output file

### LogBuffer

A custom Logback `AppenderBase` that copies every `ILoggingEvent` into a
fixed-size circular buffer (1000 lines). The Javalin log endpoint streams
the buffer contents to the browser via Server-Sent Events. New events are
pushed as they arrive; the browser reconnects automatically on disconnect.

---

## Technology Stack

| Concern           | Library / Version       |
|-------------------|-------------------------|
| FIX engine        | QuickFIX/J 2.3.x        |
| Web framework     | Javalin 6.x             |
| UI styling        | Pico.css (CDN) + custom |
| Scripting         | Groovy 4.x              |
| Logging           | Logback 1.5.x           |
| Config parsing    | jackson-dataformat-toml |
| Build             | Maven                   |
| Java version      | 17                      |

No Spring. No Freemarker. HTML pages are plain static files served from
`src/main/resources/web/`.

---

## TLS Version Constraint

The gateway's TLS listener is capped at **TLS 1.2** even though Java 21 and
OpenSSL 3.0 both support TLS 1.3. The reason is a fundamental incompatibility
in Apache MINA 2.1.x's `SslFilter` (tracked as
[DIRMINA-1149](https://issues.apache.org/jira/browse/DIRMINA-1149), fixed in
MINA 2.2.0).

In TLS 1.3, after the handshake the server sends one or more `NewSessionTicket`
records. When `SSLEngine.unwrap()` processes these records it can return
`HandshakeStatus.NEED_WRAP`, indicating the engine needs to write a response
before it can continue delivering application data. MINA 2.1.x does not loop
`unwrap/wrap` correctly in this state: it stops reading from the socket and
never delivers the server's subsequent application data (the FIX Logon
response) to the QuickFIX/J session layer. The result is that the FIX client
times out with "Timed out waiting for logon response" on every connection
attempt.

The cap is applied in `TlsContext::apply_common_tls_options` in
`libraries/pubsub_itc_fw/src/TlsContext.cpp`:

```cpp
SSL_CTX_set_max_proto_version(context, TLS1_2_VERSION);
```

TLS 1.2 with `ECDHE-RSA-AES256-GCM-SHA384` is still strong for a dev/POC
environment. To move to TLS 1.3, upgrade QuickFIX/J to a version that depends
on MINA 2.2.0 or later, then remove the `set_max_proto_version` call.

---

## Project Structure

```
java/fix-test-client/
├── pom.xml
├── DESIGN.md
├── config/
│   └── app.toml                    application config (host, port, paths)
├── scripts/                        saved Groovy scripts directory
├── src/
│   └── main/
│       ├── java/
│       │   └── com/pubsub/fixtestclient/
│       │       ├── Main.java
│       │       ├── Config.java
│       │       ├── fix/
│       │       │   ├── FixEngine.java
│       │       │   └── FixApplication.java
│       │       ├── script/
│       │       │   ├── ScriptRunner.java
│       │       │   ├── ScriptState.java
│       │       │   └── FixSessionBinding.java
│       │       ├── capture/
│       │       │   └── MessageCapture.java
│       │       ├── log/
│       │       │   └── LogBuffer.java
│       │       └── web/
│       │           └── WebServer.java
│       └── resources/
│           ├── logback.xml
│           └── web/
│               ├── session.html
│               ├── script.html
│               ├── messages.html
│               ├── config.html
│               └── logs.html
└── output/                         created at runtime
```

---

## Configuration

### `config/app.toml`

```toml
[server]
port = ${fix_test_client_server_port}

[fix]
gateway_host         = "127.0.0.1"
gateway_port         = ${fix_test_client_fix_gateway_port}
target_comp_id       = "GATEWAY"
trust_store_path     = "config/fix_gateway_trust.jks"
trust_store_password = "pubsub_dev"

[capture]
output_dir = "output"

[scripts]
scripts_dir = "scripts"
```

All QuickFIX/J session settings (`TargetCompID`, host, port, etc.) are built
programmatically by `FixEngine.buildSettings()` using the values from this
file. There is no separate QuickFIX/J `session.cfg`.

---

## UI Design

### General Principles

- **High visibility**: nothing hidden behind hover states, collapsed panels, or
  hamburger menus. All navigation and status is always on screen.
- **Explicit over implicit**: current location is shown as a page heading in
  plain text, not inferred from button styling.
- **Buttons look like buttons**: raised 3D appearance using box-shadow so they
  are immediately recognisable as interactive. The pressed-in (inset shadow)
  state is visually obvious without requiring the user to learn a colour convention.

### Button Styling

```css
nav button, .action-button {
    box-shadow: 2px 2px 4px #888, -1px -1px 2px #fff;
    border: 1px solid #999;
    background: #e8e8e8;
    padding: 6px 14px;
}

nav button:active, nav button.pressed {
    box-shadow: inset 2px 2px 4px #888;
    border: 1px solid #777;
    background: #d0d0d0;
}
```

### Page Chrome (every page)

Two fixed rows appear at the top of every page:

```
┌─────────────────────────────────────────────────────────────────┐
│  [ Session ]  [ Script ]  [ Messages ]  [ Config ]  [ Logs ]    │
├─────────────────────────────────────────────────────────────────┤
│  ● LOGGED ON   CLIENT → GATEWAY   19:26:39   out: 42   in: 1002 │
└─────────────────────────────────────────────────────────────────┘
```

Row 1 — navigation buttons. All five always visible. Buttons use the raised
3D style; no button is visually distinguished as "active" — the page heading
below serves that purpose unambiguously.

Row 2 — session status strip. Updates in real time. When not logged on:
```
  ○ NOT LOGGED ON
```

### Session Page

**Before logon** (landing page):

If arriving after a logout, a summary of the ended session is shown above the
form:

```
  Last session
  ─────────────────────────────────────────────
  Logged in as   CLIENT
  Started        2026-06-02  19:26:39
  Ended          2026-06-02  19:36:52
  Duration       10 minutes 13 seconds
```

Login form:

```
  Session
  ─────────────────────────────────────────────

  SenderCompID      [ CLIENT          ]
  Starting seq num  [ ] Override    [ 1    ]

                    [ Log On ]
```

Starting sequence number defaults to whatever QuickFIX/J has stored. The
override checkbox enables the input field.

**After logon:**

```
  Session
  ─────────────────────────────────────────────

  Logged in as     CLIENT
  Gateway          GATEWAY  @  127.0.0.1:9879
  Logged in at     2026-06-02  19:26:39
  Duration         00:10:23              ← ticking live
  Starting seq     1
  Outbound seq     42                    ← updates live
  Inbound seq      1,002                 ← updates live

  [ Log Out ]
```

### Script Page

```
  Script
  ─────────────────────────────────────────────

  State:    ○ IDLE
  Capture:  ○ NOT CAPTURING

  [ untitled.groovy        ]   [ Load ]  [ Save ]  [ New ]

  ┌──────────────────────────────────────────────────────┐
  │                                                      │
  │  (script editor)                                     │
  │                                                      │
  └──────────────────────────────────────────────────────┘

  [ Run ]    [ Stop ]

  Output
  ─────────────────────────────────────────────
  ┌──────────────────────────────────────────────────────┐
  │  (live output, streams as script runs)               │
  └──────────────────────────────────────────────────────┘
```

While running:
```
  State:    ● RUNNING
  Capture:  ● CAPTURING → output/20260602-192639.log   (4,312 messages written)
```

On completion:
```
  State:    ● COMPLETED
```

On failure:
```
  State:    ✗ FAILED — groovy.lang.MissingMethodException: No signature of ...
```

- Stop is greyed and unclickable when idle; Run is greyed when running.
- Script content persists server-side across page navigation.
- Load shows the list of `.groovy` files in the configured scripts directory.
- Save writes to that directory using the name shown in the filename field.
- New clears the editor and resets the filename to `untitled.groovy`.

### Messages Page

A form for sending a New Order Single sits above the blotter. The blotter
accumulates all outbound NOS and inbound ER messages for the session.

**Send form:**
```
  New Order Single
  ──────────────────
  ClOrdID  [ ORD-001 ]   Symbol  [ BHP   ]   Side  [ Buy   ]
  OrdType  [ Limit   ]   Qty     [ 100   ]   Price [ 10.50 ]

  [ Send ]
```

**Blotter:**

One row per message — each NOS and each ER gets its own row. The blotter
persists for the duration of the session and survives page navigation.

Columns:
```
Time | Dir | SeqNum | ClOrdID | OrderID | ExecID | ExecType | OrdStatus |
Symbol | Side | OrdQty | Price | OrdType | CumQty | LeavesQty
```

Row colouring:
- NOS rows: neutral (no colour)
- ER rows — whole row coloured by OrdStatus:
  - Filled: green
  - Partial fill: amber
  - Rejected / cancelled: red

ClOrdID is the correlation key linking each NOS row to its ER rows.
Optional fields (OrdQty, Price, OrdType) show blank when absent.

### Config Page

Read-only display of the two config files as plain text:

```
  Config
  ─────────────────────────────────────────────

  app.toml
  ─────────
  (file contents)

  Edit this file on disk and restart to apply changes.
```

### Logs Page

Full-page live log stream delivered via SSE. The last 1000 lines are shown on
page load; new entries append at the bottom as they arrive.

A **Pause** button stops auto-scroll so the user can read without the page
jumping. The stream continues arriving in the background. Unpausing jumps to
the current position.

---

## Scripting Examples

### Basic logon / send / logout

```groovy
session.logon()
sleep(2000)

def nos = fix.newOrderSingle()
nos.set(new quickfix.field.ClOrdID("ORD-001"))
nos.set(new quickfix.field.Symbol("BHP"))
nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
nos.set(new quickfix.field.OrderQty(100))
nos.set(new quickfix.field.Price(10.50))
nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))

session.send(nos)
sleep(5000)
session.logout()
```

### High-volume order submission

```groovy
session.logon()
sleep(2000)

1000.times { i ->
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.ClOrdID("ORD-${i}"))
    nos.set(new quickfix.field.Symbol("BHP"))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(100))
    nos.set(new quickfix.field.Price(10.50))
    nos.set(new quickfix.field.OrdType(quickfix.field.OrdType.LIMIT))
    session.send(nos)
}

sleep(10000)
session.logout()
```

### Sequence number recovery

```groovy
session.setNextOutgoingSeqNum(50)
session.logon()
sleep(5000)
session.disconnect()      // abrupt — no Logout
sleep(2000)
session.logon()           // reconnect; gateway will send ResendRequest
sleep(5000)
session.logout()
```

---

## Output File Format

```
=== 2026-06-02T19:26:39.373Z  INBOUND  ExecutionReport (8) ===
BeginString     (8):  FIXT.1.1
BodyLength      (9):  144
MsgType        (35):  8
SenderCompID   (49):  GATEWAY
TargetCompID   (56):  CLIENT
MsgSeqNum      (34):  2
SendingTime    (52):  20260602-19:26:39
ClOrdID        (11):  ORD-001
OrderID        (37):  ME-ORD-001
ExecID         (17):  ME-EXEC-001
ExecType      (150):  F
OrdStatus      (39):  2
Symbol         (55):  BHP
Side           (54):  1
OrderQty       (38):  100
Price          (44):  10.50
CumQty         (14):  100
LeavesQty     (151):  0
Checksum       (10):  141
```

---

## Build and Run

```bash
cd java/fix-test-client
mvn package
java -jar target/fix-test-client-*.jar
# open http://localhost:8081
```

The fat JAR is produced by `maven-shade-plugin`, consistent with `admin-service`.

---

## Out of Scope (Initial Release)

- Order Cancel Request, Order Cancel Replace (scripting only, no dedicated UI form)
- Market data
- Multi-session
- Authentication / authorisation
- Docker / Kubernetes packaging
- Automated assertion framework
