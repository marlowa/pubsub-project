
# ApplicationThread ITC Message System — Complete Design Overview

## 1. Architectural Goal

Each `ApplicationThread` is a **CPU‑pinned, busy‑wait, message‑driven active object**.

- It owns a **lock‑free MPSC queue** (`LockFreeMessageQueue<EventMessage>`).
- It runs a **busy‑wait loop** in `run_internal()` that:
  - repeatedly tries to dequeue messages
  - dispatches them via `process_message(EventMessage&)`
  - exits when `is_running_` becomes false
- It is designed for:
  - ultra‑low latency
  - no blocking
  - no syscalls in the hot path
  - predictable behavior under load

The system must support:

- Pool‑allocated ITC payloads via `ExpandablePoolAllocator<T>`
- Kafka‑allocated payloads via librdkafka
- Per‑thread message type systems
- Clean shutdown via TERM messages
- No `std::variant`, no `std::visit`, no RTTI, no virtual dispatch

---

## 2. EventMessage — Pointer‑Based Envelope

`EventMessage` is a **tiny, non‑owning envelope** around event metadata and a raw payload pointer.

Key fields:

- `EventType type` — coarse category (e.g. `InterthreadCommunication`, `Timer`, `Termination`, etc.)
- `int payload_size` — size in bytes of the payload (if any)
- `TimerID timer_id` — used for timer events
- `std::string reason` — used for termination events
- `ThreadID originating_thread_id` — used for ITC messages
- `const uint8_t* payload_` — raw pointer to payload bytes (may be `nullptr`)
- `int itc_message_type_` — **thread‑local ITC subtype**, meaningful only for `EventType::InterthreadCommunication`

Design properties:

- **No inline payload storage** — keeps `EventMessage` small and cache‑friendly.
- **Non‑owning pointer** — the receiving thread is responsible for freeing the payload.
- **Factory methods** (`create_itc_message`, `create_timer_event`, `create_termination_event`, etc.) ensure consistent construction.
- `get_as<T>()` is a low‑level, unsafe reinterpret cast, to be used only after checking type and subtype.

Payload memory can come from:

- `ExpandablePoolAllocator<T>` (normal ITC case)
- librdkafka (Kafka case)
- no payload at all (TERM, INIT, etc.)

---

## 3. Why No Global ITC Enum

A single global enum for all ITC message types would be:

- Huge and ever‑growing
- Cross‑team coupled
- Hard to maintain
- A merge‑conflict magnet
- Full of unrelated message types
- A design smell in a modular system

This approach was **rejected**.

---

## 4. Per‑Thread ITC Message Registries

Instead of a global enum, **each `ApplicationThread` subclass defines its own ITC message universe**.

For each thread:

1. It defines a **local enum** of ITC message types.
2. It defines a **traits registry** mapping each message type to:
   - payload C++ type
   - allocation source (pool / Kafka / none)
   - destroy function
   - (optionally) expected payload size
3. It defines how to **decode and dispatch** messages in `process_message(EventMessage&)`.
4. It defines how to **destroy** payloads after processing.

This keeps message semantics **local to the receiving thread** and avoids a global “monster enum”.

---

## 5. ITC Message Flow — End‑to‑End

### 5.1 Sender Side (Thread A → Thread B)

1. **Allocate payload memory**:
   - Normal ITC:
     - Use `ExpandablePoolAllocator<PayloadType>` owned by the sender or receiver (depending on design).
   - Kafka:
     - librdkafka allocates a buffer on the heap and returns a pointer.

2. **Construct or serialize payload** into that memory:
   - For pool‑allocated payloads, construct a C++ object in place.
   - For Kafka, the payload is already in the buffer provided by librdkafka.

3. **Create an `EventMessage`** using `EventMessage::create_itc_message(...)`:
   - `EventType::InterthreadCommunication`
   - `originating_thread_id` set to the sender’s `ThreadID`
   - `payload_` set to the payload pointer
   - `payload_size` set appropriately
   - `itc_message_type_` set to an integer representing the **receiver’s local ITC message type** (e.g. cast from the receiver’s enum).

4. **Enqueue the message** into the receiver’s queue:
   - Use `post_message(target_thread_id, EventMessage msg)` on the sender side.
   - The Reactor or some routing mechanism maps `target_thread_id` to the correct `ApplicationThread` instance and enqueues into its `LockFreeMessageQueue<EventMessage>`.

---

### 5.2 Queue Behavior

- The queue is **MPSC**:
  - Multiple producers (Reactor, other threads)
  - Single consumer (the owning `ApplicationThread`)
- It is **lock‑free** for dequeue and **wait‑free** for enqueue.
- It supports **watermarks**:
  - High watermark: producer‑side handler when queue grows too large.
  - Low watermark: consumer‑side handler when queue drains below a threshold.
- It supports **shutdown**:
  - After shutdown, enqueue attempts are ignored (messages dropped).
  - During shutdown, remaining messages are drained and discarded.

---

### 5.3 Receiver Side (ApplicationThread)

The receiver is an `ApplicationThread` subclass.

#### 5.3.1 Busy‑Wait Loop in `run_internal()`

Conceptually:

- Log “starting thread”
- While `is_running_` is true:
  - Try to dequeue an `EventMessage` from `message_queue_`
  - If a message is available:
    - Call `process_message(EventMessage&)`
- Log “thread shutting down”

This loop:

- Is **single‑consumer** of the queue.
- Is **busy‑wait** (no blocking, no syscalls).
- Is intended to run on a **pinned CPU core** for predictable latency.

#### 5.3.2 `run()` Wrapper

- Calls `run_internal()` inside a `try/catch`.
- On normal exit:
  - Sets `is_running_ = false`.
  - Logs normal termination.
- On exception:
  - Logs abnormal termination.
  - Notifies the Reactor (e.g. `reactor_.shutdown(...)`).
  - Ensures `is_running_` is cleared.

---

## 6. Per‑Thread ITC Registry — Conceptual Shape

Each `ApplicationThread` subclass defines:

### 6.1 A Local Enum

Example shape (conceptual):

- `enum class MyThreadMsg { Terminate, PriceUpdate, KafkaMessage, ... };`

This enum is **local to the thread** and describes only the messages that this thread can receive.

### 6.2 Traits for Each Message Type

For each enum value, the thread defines traits that specify:

- **Payload type** — the C++ type the payload pointer actually points to.
- **Allocation source** — where the memory came from:
  - Pool (via `ExpandablePoolAllocator<T>`)
  - Kafka heap (via librdkafka)
  - None (for TERM, INIT, etc.)
- **Destroy function** — how to free the payload:
  - Return to pool
  - Call librdkafka’s free function
  - Do nothing
- **Optional metadata** — e.g. expected payload size for debugging.

This forms the **per‑thread ITC message registry**.

### 6.3 Mapping `itc_message_type_` to Traits

`EventMessage` stores `itc_message_type_` as an `int`.

- The sender sets this to a value that the **receiver’s enum** understands.
- The receiver interprets this integer as its own enum (e.g. via cast or lookup).
- The receiver uses that enum value to select the correct traits.

This keeps `EventMessage` generic and small, while allowing each thread to have a rich, type‑safe message system.

---

## 7. Decode + Dispatch + Destroy on Receiver

Inside the receiver’s `process_message(EventMessage& msg)`:

1. **Check the coarse event type**:
   - If `msg.type() != EventType::InterthreadCommunication`, handle other event categories (timers, termination, etc.) separately.

2. **Interpret the ITC subtype**:
   - Read `msg.itc_message_type()`.
   - Map this integer to the thread’s local enum (e.g. `MyThreadMsg`).
   - Use the enum value to select the correct traits.

3. **Decode the payload pointer**:
   - Use the traits’ payload type to reinterpret `msg.payload()` as the correct C++ type.
   - This is where `get_as<T>()` or an equivalent cast is used, but only after the subtype has been validated.

4. **Call the appropriate handler**:
   - Dispatch to a handler function or method that takes the strongly‑typed payload.
   - For example: `handle_price_update(const PriceUpdate&)`, `handle_kafka_message(const KafkaPayload&)`, etc.

5. **Destroy the payload**:
   - After the handler returns, use the traits’ destroy function to free the payload:
     - If pool‑allocated: return to `ExpandablePoolAllocator<T>`.
     - If Kafka‑allocated: call librdkafka’s free function.
     - If no payload: do nothing.

This sequence ensures:

- Type‑safe decoding (by construction).
- Correct memory ownership and destruction.
- No `std::variant`, no `std::visit`, no RTTI, no virtual dispatch.

---

## 8. Kafka Special Case

Kafka messages are special because:

- librdkafka allocates the buffer on the heap.
- The payload is an Avro binary blob or similar.
- The memory must be freed using librdkafka’s API.
- It cannot be moved into your pool.

Only the **Kafka ingestion thread** defines a message type for these.

In that thread’s registry:

- The payload type is something like `KafkaPayload` (a struct wrapping the librdkafka pointer and metadata).
- The allocation source is `KafkaHeap`.
- The destroy function calls the appropriate librdkafka free function.

Other threads:

- Never see this message type.
- Never need to know how to free Kafka memory.
- Never include librdkafka headers.

This keeps Kafka concerns **isolated** to the thread that actually deals with Kafka.

---

## 9. TERM Messages and Clean Shutdown

TERM is just another ITC message type in the thread’s local enum.

Traits for TERM:

- No payload.
- No allocation source.
- No destroy function.

In `process_message(EventMessage& msg)`:

- When the subtype is TERM:
  - The thread sets `is_running_ = false`.
  - The busy‑wait loop in `run_internal()` eventually sees `is_running_ == false` and exits.
  - `run()` then logs termination and returns.

This allows:

- Clean shutdown of a thread via an ITC message.
- Unit tests to stop a thread by sending a TERM message.

---

## 10. Unit Test Concept — Sending TERM to Stop a Thread

Conceptual flow for a unit test that stops a thread via TERM:

1. **Construct a logger** suitable for tests (e.g. console‑only or silent).
2. **Construct a Reactor** (or a minimal stub, depending on test scope).
3. **Construct a concrete `ApplicationThread` subclass** (e.g. `TestThread`) that:
   - Implements `run()` (or uses the existing `run_internal()` pattern).
   - Implements `process_message(EventMessage&)`.
   - Defines a local ITC enum including `Terminate`.
   - Defines a registry/traits for its ITC messages.
4. **Start the thread**:
   - Call `start()` on the `ApplicationThread`, which creates the underlying `std::thread` and runs `run()`.
5. **Create a TERM `EventMessage`**:
   - Use `EventMessage::create_itc_message(...)` with:
     - `EventType::InterthreadCommunication`
     - `itc_message_type_` set to the integer corresponding to the thread’s local `Terminate` enum value.
     - `payload_ = nullptr`
     - `payload_size = 0`
     - `originating_thread_id` set appropriately.
6. **Post the TERM message**:
   - Use `post_message(target_thread_id, std::move(term_msg))`.
   - The message is enqueued into the thread’s `LockFreeMessageQueue<EventMessage>`.
7. **Thread processes TERM**:
   - The busy‑wait loop in `run_internal()` dequeues the TERM message.
   - `process_message()` sees the subtype `Terminate`.
   - It sets `is_running_ = false`.
8. **Thread exits**:
   - The loop in `run_internal()` exits.
   - `run()` logs termination and returns.
9. **Test joins the thread**:
   - The test calls `join_with_timeout(...)` or similar.
   - Asserts that the thread terminated cleanly.

This test validates:

- ITC message routing.
- TERM handling.
- Clean shutdown behavior.
- Correct integration of `EventMessage`, queue, and `ApplicationThread`.

---

## 11. Why This Design Is a Good Fit

This design satisfies all the original constraints and goals:

- **No global monster enum** — each thread defines its own ITC message types.
- **No `std::variant` / `std::visit`** — decoding is done via traits and explicit casts.
- **No RTTI or virtual dispatch** — everything is resolved via enums and traits.
- **Tiny `EventMessage`** — only metadata + pointer, no inline payload.
- **Fast lock‑free queue** — small messages, pointer‑based, ideal for MPSC.
- **Clear ownership model** — receiver frees payload using traits.
- **Supports pool‑allocated payloads** — via `ExpandablePoolAllocator<T>`.
- **Supports Kafka heap payloads** — via librdkafka’s allocation and free functions.
- **Supports TERM and other system events** — via `EventType` and local ITC enums.
- **Keeps concerns local** — each thread owns its own message universe and registry.
- **Matches the busy‑wait, CPU‑pinned model** — no blocking, no syscalls in the hot path.

This is the complete, coherent design for your `ApplicationThread` ITC message system.
