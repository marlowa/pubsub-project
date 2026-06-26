# Secure Communications

## TLS

### Why TLS 1.2 (not 1.3)

<!-- MINA 2.1.x incompatibility with TLS 1.3 NewSessionTicket handling;
     cap applied in TlsContext::apply_common_tls_options. -->

### TLS Context

<!-- How TlsContext is configured; certificate and key loading; trust store. -->

### Integration with the Reactor

<!-- How TLS sits on top of the socket layer; EPOLLOUT after flush_wbio EAGAIN. -->

## SCRAM Authentication

### Overview

<!-- SCRAM-SHA-256; why authentication is a gateway concern, not a FIX client concern. -->

### Protocol Flow

<!-- Challenge/response exchange between gateway and authentication service. -->

### Credential Storage

<!-- How credentials are stored in PostgreSQL; export_credentials.py. -->

## See Also

- [Socket Communications](socket_comms.md)
- [Order Gateway](../applications/order_gateway.md)
