
# Messages Between Threads

Every event an `ApplicationThread` receives arrives as an `EventMessage` on its
`LockFreeMessageQueue<EventMessage>`: messages from other threads, timers, inbound PDUs and raw
bytes, connection events, and the start-up and shutdown events. This document describes that
envelope, which event types it carries and who owns each payload, and how one thread sends a
message to another. The queue itself, and how the thread waits for work, are in
[Threading](threading.md).

---

## 1. EventMessage

`EventMessage` is a non-owning envelope: it carries metadata and a raw pointer to a payload,
never the payload itself. All messages are made by static factory methods
(`create_itc_message`, `create_timer_event`, `create_termination_event`,
`create_framework_pdu_message` and so on), so that each type is always constructed with the
fields it needs.

Its header holds:

- `EventType type` — which kind of event this is (see the table below)
- `int payload_size` — size in bytes of the payload, if there is one
- `int64_t tail_position` — for `RawSocketCommunication`
- `TimerID timer_id` — for `Timer`
- `std::string reason` — for `Termination`, `ConnectionFailed` and `ConnectionLost`
- `ThreadID originating_thread_id` — for `InterthreadCommunication`
- `ConnectionID connection_id` — for the connection events

and alongside the header:

- `const uint8_t* payload_` — the payload, or `nullptr`
- `SlabHandle slab_id_` — for `FrameworkPdu`, the handle of the inbound slab chunk holding the
  payload; `invalid_slab_handle` otherwise
- `int16_t pdu_id_` and `int64_t seq_no_` — for `PubSubCommunication`
- `std::shared_ptr<MirroredBuffer> raw_buffer_owner_` — for `RawSocketCommunication`, keeps the
  receive buffer alive while the message refers into it, even if the connection's handler has
  been destroyed; empty for every other type
- `int64_t enqueued_ns_` — the monotonic time at which the message was put on the receiving
  thread's queue, used for the queue-latency histogram
- `int itc_message_type_` — see section 4

Because of the `std::string` and the `shared_ptr`, an `EventMessage` is not a small plain
structure, and it is moved, not copied, into the queue.

`get_as<T>()` reinterprets the payload pointer as a `const T&`. It checks nothing, so it must
only be used once the event type has been checked.

### Event types and who owns the payload

| `EventType` | Payload | Who frees it |
|---|---|---|
| `Initial`, `AppReady`, `Termination`, `Timer` | None | — |
| `InterthreadCommunication` | Whatever the sender points at, or nothing | Agreed between sender and receiver; the framework does not free it |
| `FrameworkPdu` | A chunk of the reactor's inbound slab allocator | The receiving thread, by calling `release_pdu_payload(msg)` once it has finished. Not doing so leaks the chunk |
| `PubSubCommunication` | A view into the slab chunk of the page being delivered | The framework. The view is valid only during the `on_pubsub_message()` call, so the receiver copies anything it needs to keep |
| `RawSocketCommunication` | A view into the connection's `MirroredBuffer` | Released by the application telling the reactor how many bytes it has consumed (see [Socket Comms](socket_comms.md)) |
| `ConnectionEstablished`, `ConnectionFailed`, `ConnectionLost`, `ConnectionWritable` | None | — |

`ApplicationThread::process_message()` calls the matching `on_...` callback for each type; the
list of callbacks is in [Threading](threading.md#threading_callbacks).

---

## 2. Sending a Message to Another Thread

A thread sends a message to another with `post_message(target_thread_id, message)`, having
built it with `EventMessage::create_itc_message(originating_thread_id, data, size)`.

- If the target is the sending thread itself, the message goes straight onto its own queue.
- Otherwise `post_message()` calls `Reactor::route_message()`, which finds the target thread by
  its `ThreadID` and calls its `enqueue()`. `enqueue()` stamps `enqueued_ns_`, puts the
  message on the queue, and writes to the target's eventfd to wake it.

`route_message()` does not always deliver. It silently drops the message if the target
`ThreadID` is not registered, if the target is not running, if the target is shutting down or
has terminated, or if the originating thread is not registered or not running. It throws
`PubSubItcException` if the target is running but not yet `Operational`, and
`PreconditionAssertion` if any message other than the four events the reactor itself sends
(`Initial`, `AppReady`, `Timer` and `Termination`) is posted before the reactor has finished
initialising. Looking up the originating thread takes the reactor's thread-registry mutex.

On the receiving side the message is delivered to the pure virtual `on_itc_message()`.

---

## 3. How the Venue Uses It

Every venue component implements `on_itc_message()` with an empty body, and no component calls
`post_message()` or `create_itc_message()`. A component's application thread exchanges its
work with other processes as PDUs over TCP, and receives everything else as reactor events.
Thread-to-thread messages are used today only by the framework's own tests.

---

## 4. Message Subtypes: Designed, Not Implemented

`EventMessage` has an `int itc_message_type_` field, read by `itc_message_type()`, which is
meant to say which kind of `InterthreadCommunication` message this is. **Nothing can set it**:
no factory method takes a subtype and there is no setter, so it is always `-1`. A receiver
that needs to tell message kinds apart has to do it from the payload.

The intended design, which is not built, is this.

**No global enum of message types.** A single enum listing every message any thread can
receive would keep growing, would couple unrelated components to each other, and would be
edited by everybody. Instead, each `ApplicationThread` subclass would define its own enum of
the messages it can receive, and the sender would set `itc_message_type_` to a value of the
*receiver's* enum.

**A table of traits per receiving thread.** For each value of its enum, the receiving thread
would record the C++ type the payload points to, where the memory came from, and how to free
it. Its `on_itc_message()` would look the subtype up, cast the payload to the recorded type,
call the handler for that type, and then free the payload as the traits say. This gives typed
handling without `std::variant`, `std::visit`, RTTI or virtual dispatch on the message.

The design also allowed for payloads allocated outside the framework, such as a message buffer
owned by a client library that must be freed through that library's own call. Under the
per-thread traits, only the thread that receives such messages would need to know how to free
them. The project does not use any such library.

---

## See Also

- [Threading](threading.md) — the queue, the run loop, the lifecycle and the callbacks
- [Allocators](allocators.md) — the pools behind the queue nodes and the slabs behind PDU payloads
