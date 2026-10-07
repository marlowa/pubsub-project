# FIX Test Client {#fix_test_client}

## Role

A web application for driving the venue by hand or by script, through either gateway: FIX 5.0 SP2
to the FIX order gateway, or the binary protocol to the binary order gateway. It holds one session
at a time, for one user.

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
| `MaskingLog` | Wraps QuickFIX/J's log and replaces the value of `Password` (554) with `***` before a message is written |
| `BinaryEngine` | The session with the binary gateway: a socket, the binary `Logon`, and a thread reading reports. There are no sequence numbers or heartbeats to manage |
| `GatewaySelector` | Which gateway the one live session uses, so the order form, cancel and blotter follow it |
| `ScriptRunner` | Executes Groovy scripts in a dedicated thread via `GroovyShell`; binds `session`, `fix`, `out` and `sleep` |
| `Main` | Reads `app.toml`, builds the objects above, and starts Javalin with the routes for the six pages and their API, serving static files from the classpath `/web/` |

## Session Management

Session configuration is built programmatically from `app.toml` — there is no separate
`session.cfg` file. `StartTime=00:00:00` / `EndTime=00:00:00` are required so QuickFIX/J
does not reject startup with `ConfigError: StartTime not defined`.

**TLS note:** QuickFIX/J's MINA `SslFilter` does not handle TLS 1.3 `NewSessionTicket`
records correctly and deadlocks waiting for a response it never sends. The gateway's
`TlsContext` caps at TLS 1.2 to work around this. See
[Secure Communications](../operations/secure_comms.md).

## Logon modes {#ftc_logon_modes}

Each FIX endpoint in the configuration has a logon mode. **Standard** is the default and is what
this venue's FIX gateways expect. **Proprietary** is for connecting the client to a FIX gateway
whose logon departs from the standard in the ways below. The development configuration has one such
endpoint, `fix-proprietary`, on port 30994 of the FIX gateway's host, and nothing in this venue
listens on that port; it is kept so that the client can be pointed at such a gateway.

With `logon_mode = "proprietary"` on an endpoint, the client:

- **connects in plain TCP only.** A proprietary logon with TLS is refused, and the logon page
  disables TLS for that endpoint;
- **stamps `SendingTime` (52) and `TransactTime` (60) with nanosecond precision** on every
  message, from `NanoClock`, which derives nanosecond wall time from the millisecond clock and a
  `System.nanoTime()` difference, so the precision does not depend on the platform's clock;
- **sends the password in `EncryptedPassword` (1402)** with `EncryptedPasswordMethod` (1400) set to
  101, instead of in `Password` (554). The password is sent as written for now: encrypting it
  before it is placed in the field is still to be done (a TODO in `FixApplication`);
- **does not ask to reset sequence numbers.** `ResetSeqNumFlag` (141) is removed and the session
  is not reset at logon, so numbering carries on from the client's message store, or from the
  starting number given on the logon form or with `setNextOutgoingSeqNum`;
- **sends `NextExpectedMsgSeqNum` (789)** in the Logon.

`TargetCompID` can be set on the logon form for either mode, and defaults to `GATEWAY`.

## UI (Six Pages)

All pages display a persistent nav bar and a live session status strip.

| Page | Purpose |
|------|---------|
| **Session** | Logon form, with the choice of gateway endpoint (FIX or binary, instance `a` or `b`) and an optional starting sequence number; live post-logon detail (ticking duration, live sequence counters); last-session summary shown after logout |
| **Script** | Groovy editor with Load/Save/New; state badge (IDLE / RUNNING / COMPLETED / FAILED); live output; capture status |
| **Messages** | New Order Single send form; blotter table with row colouring by `OrdStatus` (filled=green, partial=amber, rejected/cancelled=red); Cancel button on each NOS row pre-fills the cancel form. Works with either gateway |
| **Raw** | Sends a FIX message typed by hand. FIX sessions only: there is no hand-typed binary message |
| **Config** | Read-only display of `app.toml` |
| **Logs** | SSE log stream with Pause/Resume; last 1000 lines shown on load |

## Advanced NOS Fields {#ftc_advanced_nos}

The New Order Single form on the Messages page has a row of six fields --- ClOrdID, Symbol,
Side, OrdType, Qty, Price --- and, in a collapsed `<details>` block beneath it, the optional fields
below, so the common order needs only the first row and the rest is one click away.

| FIX tag | Label | Control | DSL field | Conditional rule |
|---|---|---|---|---|
| 59  | TimeInForce   | select (Day, GTC, IOC, FOK, GTD) | `time_in_force` | absence implies Day |
| 126 | ExpireTime    | datetime | `expire_time` | required when TimeInForce=GTD; enabled only then |
| 99  | StopPx        | number | `stop_px` | for Stop / StopLimit OrdType |
| 1   | Account       | text | `account` | often venue-required |
| 100 | ExDestination | text | `ex_destination` | routing destination |
| 18  | ExecInst      | text | `exec_inst` | single-valued in this topic |
| 110 | MinQty        | number | `min_qty` | minimum acceptable fill |
| 111 | MaxFloor      | number | `max_floor` | iceberg display quantity |
| 58  | Text          | text | `text` | free text |

The element ids are `f-tif`, `f-expiretime`, `f-stoppx`, `f-account`, `f-exdest`, `f-execinst`,
`f-minqty`, `f-maxfloor` and `f-text`.

<!-- verify: present java/fix-test-client/src/main/resources/web/messages.html "f-tif" -->
<!-- verify: present java/fix-test-client/src/main/resources/web/messages.html "f-minqty" -->
<!-- verify: present java/fix-test-client/src/main/resources/web/messages.html "f-maxfloor" -->
<!-- verify: present java/fix-test-client/src/main/resources/web/messages.html "f-exdest" -->

**How an optional field reaches the wire**, in the same way as the six:

1. The inputs live in `web/messages.html` inside the `<details>` block.
2. `doSend()` collects them and adds them to the POST body **only when non-empty**, so an
   untouched advanced field is simply omitted.
3. `MessagesHandler` reads each with `ctx.formParam(...)` and sets it on the QuickFIX
   `NewOrderSingle` **only when present**. This is the one correctness rule of the whole
   feature: an absent optional must stay absent on the wire, never be sent as an empty tag.
   Every field follows the same shape, `if (str != null && !str.isBlank())`.
4. The UI enforces the conditional rules, so the form cannot easily build a spec-invalid
   order --- selecting a TimeInForce other than GoodTillDate disables ExpireTime and clears
   it. The gateway remains the authority and still validates.

<!-- verify: present java/fix-test-client/src/main/java/com/pubsub/fixtestclient/web/MessagesHandler.java "NoUnderlyings" -->
<!-- verify: present java/fix-test-client/src/main/java/com/pubsub/fixtestclient/web/MessagesHandler.java "NoPartyIDs" -->
**Two repeating groups have controls too.** `NoUnderlyings` and `NoPartyIDs` have rows that can be
added and removed, posted as parallel lists paired by index, and `MessagesHandler` builds a QuickFIX
group per non-empty row. Nothing beyond these two has a control, because a control for every tag
would end in rebuilding the FIX dictionary as a web form; any other field is sent from the Raw page
or from a Groovy script.

## Scripting (Groovy)

Scripts run in `ScriptRunner` via `GroovyShell` with four bindings. Scripts drive FIX sessions only.

| Binding | Type | Purpose |
|---------|------|---------|
| `session` | `FixSessionBinding` | `logon(...)` to the first standard FIX endpoint, `logonTo(key, ...)` to a named endpoint, `logonProprietary(compId[, targetCompId], password)` to the first proprietary endpoint, `logout()`, `disconnect()`, `setNextOutgoingSeqNum(n)`, `isLoggedOn()`, `send(Message)` |
| `fix` | `FixHelper` | Message factory: `newOrderSingle()`, `orderCancelRequest()`, and `uniqueId()` for a fresh ClOrdID |
| `out` | `PrintWriter` | Writes to the script's output, shown live on the Script page |
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

Opens on the port in `[server] port`, 8081 in the development environment.

## Configuration

`config/app.toml` (relative to the working directory when the JAR is run); the values in `${...}`
come from the environment file when the venue is deployed.

| Section and key | Purpose |
|-----|---------|
| `[server] port` | The web interface's port |
| `[fix] target_comp_id` | The default `TargetCompID`, `GATEWAY` |
| `[fix] tls_enabled`, `trust_store_path`, `trust_store_password` | TLS to the FIX gateway, and the trust store holding the gateway's certificate |
| `[[gateway]]` | One entry per endpoint the logon page offers, in order: `key`, `label`, `protocol` (`fix` or `binary`), `host`, `port`, `tls_port` (omitted where there is no TLS listener) and `logon_mode` (`proprietary`, or omitted for standard) |
| `[capture] output_dir` | Where message captures are written |
| `[scripts] scripts_dir` | Where scripts are loaded from and saved to |
| `[web] logo_path` | A PNG to show as the logo in the corner of every page; when it is empty or cannot be read, the bundled `web/logo.png` is shown |

The development configuration lists both instances of each gateway, deliberately including
instances a comp id may not be provisioned for, so that a gateway's refusal can be shown. A member is
told its endpoints when it is provisioned, which is why they are configured here rather than
discovered from the venue's own configuration.

## See Also

- FIX Test Client detailed design — see `java/fix-test-client/DESIGN.md` in the source tree
- [Secure Communications](../operations/secure_comms.md) — TLS 1.2 cap and its cause
- [Order Gateway](fix_order_gateway.md) — the gateway this client connects to; its
  [use of `fix_codec`](fix_order_gateway.md#gw_fix_codec_migration) makes the fuller
  NewOrderSingle cheap to read, which the Advanced NOS Fields form exercises
- [FIX Codec](../fix/codec.md) — the codec library the gateway uses

The client itself is a Java application. Its internal design is documented alongside the code, in
`java/fix-test-client/DESIGN.md`.
