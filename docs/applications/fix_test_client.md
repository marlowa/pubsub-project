# FIX Test Client {#fix_test_client}

## Role

A FIX 5.0 SP2 gateway test client for interactive and scripted testing. It replaces the
`fix8` command-line tool as the primary way to exercise the order pipeline end-to-end.
Single-user, single-session web application.

**Technology stack:** Java 17, QuickFIX/J 2.3.1, Javalin 6.3.0, Groovy 4.0.21, toml4j,
Logback 1.5.x. Fat JAR via maven-shade. No Spring, and no CSS framework -- the UI is styled
by `web/style.css` alone.

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

## Proposed: Advanced NOS Fields (not yet built)

This is a design sketch, not an implemented feature. It is the UI-side follow-up to the
gateway's [migration to `fix_codec`](order_gateway.md#planned-migration-to-fix_codec-not-yet-done): once
the gateway can accept the fuller NewOrderSingle cheaply, the entry form becomes the thing
that can no longer drive it.

**Today** the New Order Single form on the Messages page exposes six fields — **ClOrdID,
Symbol, Side, OrdType, Qty, Price** (element ids `f-clordid`, `f-symbol`, `f-side`,
`f-ordtype`, `f-qty`, `f-price`). The DSL `NewOrderSingle` topic already carries many more
optional fields that no control can currently set.

**Proposed** — keep the six-field row as the default fast path and add a collapsed
`<details>` block beneath it for the optional tags. The common order is unchanged; the
advanced fields are one click away and stay out of the way when unused:

```
New Order Single
 ClOrdID [ORD-001]  Symbol [BHP]  Side [Buy v]  OrdType [Limit v]  Qty [100]  Price [10.50]  (Send)

 ▸ Advanced fields                     ← collapsed <details>, closed by default

 ── when expanded ────────────────────────────────────────────────
 ▾ Advanced fields
   TimeInForce [Day        v]   Account [____________]   ExDestination [______]
   StopPx      [____] (Stop)    ExpireTime [__________]  ExecInst [__________]
   MinQty      [____]           MaxFloor  [____]         Text     [__________]
 ──────────────────────────────────────────────────────────────────
```

The fields to surface, each of which the topic already carries, so only the gateway map and
this form need touching:

| FIX tag | Label | Control | DSL field | Conditional rule to reflect in the UI |
|---|---|---|---|---|
| 59  | TimeInForce   | select (Day, GTC, IOC, FOK, GTD) | `time_in_force` | absence implies Day |
| 126 | ExpireTime    | datetime | `expire_time` | required when TimeInForce=GTD; enable only then |
| 99  | StopPx        | number | `stop_px` | required for Stop / StopLimit OrdType; enable only then |
| 1   | Account       | text | `account` | often venue-required |
| 100 | ExDestination | text | `ex_destination` | routing destination |
| 18  | ExecInst      | text | `exec_inst` | single-valued in this topic |
| 110 | MinQty        | number | `min_qty` | minimum acceptable fill |
| 111 | MaxFloor      | number | `max_floor` | iceberg display quantity |
| 58  | Text          | text | `text` | free text |

**How it threads through** (mirrors the existing six fields):

1. Add the inputs to `web/messages.html` inside the `<details>` block.
2. `doSend()` collects them and adds them to the POST body **only when non-empty**, so an
   untouched advanced field is simply omitted.
3. `MessagesHandler` reads each with `ctx.formParam(...)` and sets it on the QuickFIX
   `NewOrderSingle` **only when present** — an absent optional must stay absent on the wire,
   never sent as an empty tag. This is the one correctness rule of the change.
4. The UI enforces the conditional rules above (enable StopPx for Stop/StopLimit, ExpireTime
   for GTD) so the form cannot easily build a spec-invalid order; the gateway remains the
   authority and still validates.

**Deliberately not surfaced.** Rarely-used or repeating-group fields are left to the existing
**raw-FIX** escape hatch and Groovy scripting rather than growing a control per tag. The goal
is to make the *common* richer order convenient, not to rebuild the whole FIX dictionary as a
web form. Sequencing: this follows the gateway migration; there is no value in the controls
until the gateway accepts the fields.

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
- [Order Gateway](order_gateway.md) — the gateway this client connects to; its
  [`fix_codec` migration](order_gateway.md#planned-migration-to-fix_codec-not-yet-done) is what motivates
  the proposed Advanced NOS Fields form
- [FIX Codec](../design/fix_codec.md) — the codec library behind that migration
