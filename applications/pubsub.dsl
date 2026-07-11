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
#  See docs/design/dsl_topic_catalog.md.
#
#  The message definitions (and their pdu ids) come from fix_equity_orders.dsl,
#  pulled in transitively so the topic members below resolve to real pdu ids.
# -----------------------------------------------------------------------------

include "fix_equity_orders.dsl"

# The "orders" topic: buy-side order flow (NewOrderSingle + OrderCancelRequest).
topic orders {
    NewOrderSingle,
    OrderCancelRequest,
}

# The "execution_reports" topic: sell-side execution reports.
topic execution_reports {
    ExecutionReport,
}
