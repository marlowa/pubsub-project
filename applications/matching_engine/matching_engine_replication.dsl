# ============================================================
#  Messages between the two matching engines of a pair
# ============================================================
#
#  Sent on the link between the two engines ([peer]), besides the
#  lease requests and grants and the role announcements, which are
#  the library's.
#
#  PDU ID range: 600-619 (between the engines, internal)
# ============================================================

# Sent by each engine to its peer when its connection to the peer's listener is established:
# the settings that change what an engine does with a command, which the two engines of a pair must share
# or apply the same commands differently (docs/availability/deterministic_matching_engine.md, section 5).
# An engine that leads, and finds its peer's settings differ from its own, says that its peer may not lead.
message EngineSettings(id=601)
    i64             instance_id
    # The most orders the engine's open-order region holds. An order arriving when it is full is refused.
    i64             order_book_region_capacity
    # A fingerprint of the secret the engine's OrderID and ExecID keys are derived from, which tells two secrets apart
    # without showing either (ReportIdentifiers::secret_fingerprint). Engines with different secrets name the same
    # report differently.
    i64             identifier_secret_fingerprint
end

# Sent by an engine that finds, on starting, that it died applying a command: what is the highest command the peer has
# applied? If the peer has applied it, or a later one, the command did not cause the death, because the two engines apply
# the same commands in the same way and the peer survived it (docs/availability/voided_commands.md, section 4.2).
message AppliedThroughQuery(id=602)
    i64             instance_id
end

# The answer to AppliedThroughQuery.
message AppliedThrough(id=603)
    i64             instance_id
    # The highest command the engine has applied, live or by a catch-up, or the position its open-order region or a
    # completed catch-up brought its book to, whichever is higher.
    i64             highest_applied
end
