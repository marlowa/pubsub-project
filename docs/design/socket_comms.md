# Socket Communications

## Design Goals

<!-- Zero-copy on all PDU paths; reactor-owned I/O; backpressure handling. -->

## PDU Framing

<!-- Wire format: length prefix, canary, payload. PduFramer and PduParser. -->

## Raw Sockets

<!-- How raw (non-PDU) socket connections work; use in the FIX gateway. -->

## Connection Management

<!-- Inbound and outbound connection lifecycles; reconnect; idle timeout. -->

## Backpressure

<!-- Read backpressure (EPOLLIN deregistration); write backpressure (EPOLLOUT). -->

## See Also

- [Secure Communications](secure_comms.md)
- [Reactor](reactor.md)
