"""The framework's PDU framing, in plain Python: the 24-byte header in front of every DSL message on a connection.

The header's layout is fixed by libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduHeader.hpp, not by the DSL, and
is in network byte order (big-endian), while the DSL message after it is little-endian. In order:

    byte_count   4 bytes, unsigned   the length of the message after the header
    pdu_id       2 bytes, signed     the DSL message id
    version      1 byte,  signed     the message version
    filler_a     1 byte              zero
    seq_no       8 bytes, signed     the sequencer's sequence number, or 0
    canary       4 bytes, unsigned   always 0xC0FFEE00
    filler_b     4 bytes             zero

The C++ parser checks only the canary, and closes the connection when it is wrong; PduReader does the same check
and raises FramingError. It runs on Python 3.8 and later, using only the standard library.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import List, Tuple

HEADER = struct.Struct(">IhbBqII")
HEADER_SIZE = HEADER.size  # pylint: disable=invalid-name
CANARY = 0xC0FFEE00

assert HEADER_SIZE == 24, "the PDU header is 24 bytes, as PduHeader.hpp asserts"


class FramingError(ValueError):
    """Bytes that are not a PDU: the canary is wrong, so the stream cannot be trusted from here on."""


@dataclass
class PduHeader:
    """The fields of a PDU header that mean something; the two fillers are always zero."""

    byte_count: int
    pdu_id: int
    version: int
    seq_no: int = 0


def encode_header(header: PduHeader) -> bytes:
    """The 24 bytes of a header, as the C++ framework writes them."""
    return HEADER.pack(header.byte_count, header.pdu_id, header.version, 0, header.seq_no, CANARY, 0)


def decode_header(buffer: bytes, position: int = 0) -> PduHeader:
    """The header at position. Raises FramingError if fewer than 24 bytes remain or the canary is wrong."""
    if len(buffer) - position < HEADER_SIZE:
        raise FramingError(f"a PDU header needs {HEADER_SIZE} bytes and {len(buffer) - position} remain")
    byte_count, pdu_id, version, _filler_a, seq_no, canary, _filler_b = HEADER.unpack_from(buffer, position)
    if canary != CANARY:
        raise FramingError(f"PDU canary is 0x{canary:08X}, not 0x{CANARY:08X}: corruption, or not a PDU stream")
    return PduHeader(byte_count=byte_count, pdu_id=pdu_id, version=version, seq_no=seq_no)


def encode_pdu(pdu_id: int, version: int, payload: bytes, seq_no: int = 0) -> bytes:
    """A whole PDU: the header for payload, then payload."""
    return encode_header(PduHeader(byte_count=len(payload), pdu_id=pdu_id, version=version, seq_no=seq_no)) + payload


class PduReader:
    """Takes the bytes of a stream as they arrive and returns each whole PDU.

    A read from a socket may return part of a PDU, or several, or the end of one and the start of the next. feed()
    keeps whatever is incomplete until the rest arrives, and returns only whole PDUs, in the order they came.
    """

    def __init__(self) -> None:
        self._pending = bytearray()

    def feed(self, data: bytes) -> List[Tuple[PduHeader, bytes]]:
        """Add data, and return every PDU it completes, as (header, message bytes)."""
        self._pending += data
        complete: List[Tuple[PduHeader, bytes]] = []
        while len(self._pending) >= HEADER_SIZE:
            header = decode_header(self._pending)
            total = HEADER_SIZE + header.byte_count
            if len(self._pending) < total:
                break
            complete.append((header, bytes(self._pending[HEADER_SIZE:total])))
            del self._pending[:total]
        return complete

    @property
    def pending_bytes(self) -> int:
        """How many bytes are held waiting for the rest of a PDU."""
        return len(self._pending)
