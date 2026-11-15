# -----------------------------------------------------------------------------
#  pubsub.dsl
#
#  Pub/sub topic catalog INDEX. This is the single source of truth for which
#  topics exist and which messages belong to each. It is consumed by the DSL
#  topics generator (generate_cpp_from_dsl.py --topics-registry/--topics-catalog)
#  to produce, in the build tree:
#    - topics_registry.hpp  (Topic enum, to_string, topic_from_name,
#                            TopicMember membership table, pdu_in_topic)
#    - topics_catalog.md     (human-readable catalog)
#
#  Identity (which topics, which member messages -> pdu ids) lives here and is
#  GENERATED downstream so it can never drift from the pdu ids. Per-topic
#  operational policy (retention, ports, ...) is hand-written TOML validated
#  against the generated registry -- it is deliberately NOT here.
#  See docs/superseded/dsl_topic_catalog.md.
#
#  The message definitions (and their pdu ids) come from fix_orders.dsl, for the
#  orders and the execution reports, and from the framework's leader_follower.dsl,
#  for TechnicalEvent. Both are pulled in so that the topic members below resolve
#  to real pdu ids. Only the registry and the catalog are generated from this
#  file, so including both defines no message twice in any program.
# -----------------------------------------------------------------------------

include "fix_orders.dsl"
include "leader_follower.dsl"

# The "orders" topic: buy-side order flow (NewOrderSingle + OrderCancelRequest).
topic orders {
    NewOrderSingle,
    OrderCancelRequest,
}

# The "execution_reports" topic: sell-side execution reports.
topic execution_reports {
    ExecutionReport,
}

# The "technical_events" topic: every change of the venue's phase, trading day, or whether trading is
# halted, each carrying the sequencer's sequence number, so that a subscriber to it and to another topic
# can tell where each change falls among the orders and reports. For the components that only need to
# know what the venue is doing (docs/venue/trading_day_plan.md, section 4.3).
topic technical_events {
    TechnicalEvent,
}
