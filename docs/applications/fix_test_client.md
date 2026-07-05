# FIX Test Client

## Role

A FIX 5.0 SP2 gateway test client for interactive and scripted testing. It replaces the
`fix8` command-line tool as the primary way to exercise the order pipeline end-to-end.
Single-user, single-session web application.

**Technology stack:** Java 17, QuickFIX/J 2.3.1, Javalin 6.3.0, Groovy 4.0.21, toml4j,
Logback 1.5.x, Pico.css. Fat JAR via maven-shade. No Spring.

## Architecture

| Component | Role |
|-----------|------|
| `FixEngine` | Wraps `SocketInitiator`; owns FIX session lifecycle; exposes `SessionStatus` |
| `FixApplication` | `quickfix.Application` implementation; routes inbound messages to blotter and capture queue via registered listener |
| `BlotterStore` | Thread-safe; accumulates all outbound NOS and inbound ER messages for the session; parses ER fields into `BlotterRow` records |
| `MessageCapture` | Writer thread drains `LinkedBlockingQueue<Message>` to a timestamped log file in `output/`; active while a script is running |
| `LogBuffer` | Logback `AppenderBase`; copies every `ILoggingEvent` into a 1000-line ring buffer; pushes new entries to SSE subscriber queues |
| `ScriptRunner` | Executes Groovy scripts in a dedicated thread via `GroovyShell`; binds `session`, `fix`, and `sleep` |
| `WebServer` (Main) | Javalin with five page sets of routes; manual DI; centralised exception handling; static files from classpath `/web/` |

## Session Management

Session configuration is built programmatically from `app.toml` — there is no separate
`session.cfg` file. `StartTime=00:00:00` / `EndTime=00:00:00` are required so QuickFIX/J
does not reject startup with `ConfigError: StartTime not defined`.

**TLS note:** QuickFIX/J's MINA `SslFilter` does not handle TLS 1.3 `NewSessionTicket`
records correctly and deadlocks waiting for a response it never sends. The gateway's
`TlsContext` caps at TLS 1.2 to work around this. See
[Secure Communications](../design/secure_comms.md).

## UI (Five Pages)

All pages display a persistent nav bar and a live session status strip.

| Page | Purpose |
|------|---------|
| **Session** | Logon form with optional seq-num override; live post-logon detail (ticking duration, live seq counters); last-session summary shown after logout |
| **Script** | Groovy editor with Load/Save/New; state badge (IDLE / RUNNING / COMPLETED / FAILED); live output; capture status |
| **Messages** | New Order Single send form; blotter table with row colouring by `OrdStatus` (filled=green, partial=amber, rejected/cancelled=red); Cancel button on each NOS row pre-fills the cancel form |
| **Config** | Read-only display of `app.toml` |
| **Logs** | SSE log stream with Pause/Resume; last 1000 lines shown on load |

## Scripting (Groovy)

Scripts run in `ScriptRunner` via `GroovyShell` with three bindings:

| Binding | Type | Purpose |
|---------|------|---------|
| `session` | `FixSessionBinding` | `logon()`, `logout()`, `send(Message)` |
| `fix` | `FixHelper` | Message factory: `newOrderSingle()`, `orderCancelRequest()` |
| `sleep` | `groovy.lang.Closure` | `sleep(ms)` — pauses script without blocking the JVM |

Example script:
```groovy
10.times {
    def nos = fix.newOrderSingle()
    nos.set(new quickfix.field.Symbol("XYZW"))
    nos.set(new quickfix.field.Side(quickfix.field.Side.BUY))
    nos.set(new quickfix.field.OrderQty(100))
    nos.set(new quickfix.field.Price(10.50))
    session.send(nos)
    sleep(50)
}
```

## Message Capture

While a script is running, `MessageCapture` drains a `LinkedBlockingQueue<Message>` to a
timestamped log file in `output/`. Each record carries direction, timestamp, and the full
FIX message. Capture is active only during script execution.

The blotter (`BlotterStore`) accumulates all messages for the entire session regardless of
capture state, and persists across script runs until the FIX session is logged out.

`BlotterRow` fields parsed from inbound ERs: `ClOrdID`, `OrigClOrdID`, `OrderID`,
`ExecID`, `ExecType`, `OrdStatus`, `Symbol`, `Side`, `OrdQty`, `Price`, `OrdType`,
`CumQty`, `LeavesQty`.

## Build and Run

```
cd java/fix-test-client && mvn package
java -jar target/fix-test-client-*.jar
```

Opens on `http://localhost:8081`.

## Configuration

`app.toml` (in the working directory when the JAR is run):

| Key | Purpose |
|-----|---------|
| `[fix] host / port` | Gateway FIX listener (default port 9879) |
| `[fix] sender_comp_id / target_comp_id` | FIX session identifiers |
| `[fix] heartbeat_interval` | FIX heartbeat interval in seconds |

## See Also

- FIX Test Client detailed design — see `java/fix-test-client/DESIGN.md` in the source tree
- [Secure Communications](../design/secure_comms.md) — TLS 1.2 cap and its cause
- [Order Gateway](order_gateway.md) — the gateway this client connects to
