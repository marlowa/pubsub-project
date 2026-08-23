#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>
#include <pubsub_itc_fw/Wal.hpp>

#include "EpochStore.hpp"
#include "GatewayIds.hpp"
#include "SequencerConfiguration.hpp"
#include "SessionIdentity.hpp"

namespace sequencer {

/**
 * @brief ApplicationThread subclass implementing the sequencer business logic.
 *
 * The sequencer is the sole writer to the matching engine's input stream. It
 * imposes a total order on all inbound order PDUs by stamping a monotonically
 * increasing sequence number and wrapping each PDU in a SequencedMessage
 * envelope before forwarding to the ME.
 *
 * Only the leader forwards to the ME. The follower receives PDUs from the
 * gateway (staying in sync) but does not forward. On promotion the follower
 * begins forwarding from the next sequence number with no gaps.
 *
 * The sequencer is the sole chokepoint for all traffic in both directions.
 * Order PDUs from gateways are sequenced and forwarded to the ME. ER PDUs
 * from the ME are forwarded back to the originating gateway. This matches
 * the Aeron cluster ingress/egress pattern exactly.
 *
 * Threading: ThreadID 1.
 */
class SequencerThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger. Must outlive this object.
     * @param[in] reactor  Owning Reactor. Must outlive this object.
     * @param[in] config   Sequencer configuration.
     */
    SequencerThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                    const SequencerConfiguration& config);

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;
    bool prioritise_data_over_timers() const override {
        return true;
    }

  private:
    const SequencerConfiguration& config_;

    // Precomputed inbound service name strings derived from config ports.
    // Used in on_framework_pdu_message to classify inbound PDUs without
    // constructing strings on every call.
    const std::string order_inbound_svc_;
    const std::string er_inbound_svc_;
    const std::string wal_subscriber_inbound_svc_;

    // Monotonically increasing sequence number. Incremented for every PDU
    // forwarded to the matching engine. Never resets within a process lifetime.
    int64_t next_sequence_number_{1};

    // Outbound gateway connections for ER forwarding, keyed by (protocol, instance).
    //
    // More than one gateway feeds the same book -- the ASCII FIX one and the binary one --
    // and each may run as several instances, so neither axis identifies a process on its
    // own. Protocol says which wire format the report is encoded in; instance says which
    // process to send it to. See fix_common/GatewayIds.hpp and docs/design/gateway_ha.md.
    //
    // Packed into one integer key rather than a std::pair so the map needs no custom hash.
    using GatewayKey = int32_t;

    /** @brief Packs (protocol, instance) into the key gateway_conn_ids_ is indexed by. */
    static constexpr GatewayKey gateway_key(int16_t protocol, int16_t instance) {
        return (static_cast<GatewayKey>(protocol) << 16) | static_cast<GatewayKey>(static_cast<uint16_t>(instance));
    }

    std::unordered_map<GatewayKey, pubsub_itc_fw::ConnectionID> gateway_conn_ids_;

    // (protocol, instance) pairs already reported as absent from the configuration.
    // Keeps the deployment-error message to once per pair rather than once per report.
    std::unordered_set<GatewayKey> unknown_gateways_warned_;

    /** @brief The connection for a gateway instance, or nullptr when it is not connected. */
    const pubsub_itc_fw::ConnectionID* gateway_connection(int16_t protocol, int16_t instance) const {
        auto it = gateway_conn_ids_.find(gateway_key(protocol, instance));
        if (it == gateway_conn_ids_.end() || !it->second.is_valid()) {
            return nullptr;
        }
        return &it->second;
    }

    // ConnectionID of the outbound matching-engine order connection.
    // The sequencer connects outbound to the ME's order listener on
    // matching_engine_port and forwards sequenced order PDUs over this
    // connection. Set when the outbound connection is established.
    // Starts as the ME-primary connection; on ME failover it is swapped to
    // point at ME-secondary once that instance has caught up (see
    // handle_me_position_request).
    pubsub_itc_fw::ConnectionID me_outbound_order_conn_id_;

    // Pre-warmed standby connection to ME-secondary (ha_enabled only). Kept open
    // but not used for order forwarding until ME-secondary promotes itself and
    // sends a MePositionRequest, at which point it is promoted to
    // me_outbound_order_conn_id_.
    pubsub_itc_fw::ConnectionID me_secondary_standby_conn_id_;

    // ConnectionIDs of the outbound peer and arbiter connections.
    pubsub_itc_fw::ConnectionID peer_conn_id_;
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_; // inbound: peer connected to us
    pubsub_itc_fw::ConnectionID arbiter_primary_conn_id_;
    pubsub_itc_fw::ConnectionID arbiter_secondary_conn_id_;

    // instance_id of the peer sequencer, learned from StatusQuery/StatusResponse.
    // Used to populate ArbitrationReport.peer_instance_id.
    int64_t peer_instance_id_{0};

    // mmap'd on-disk write-ahead log. Opened in on_initial_event()
    // before the sequencer begins accepting connections.
    pubsub_itc_fw::Wal wal_;

    // External WAL subscriber registry and active connection set.
    // The registry tracks each subscriber's cursor for WAL truncation.
    // wal_subscriber_conn_ids_ is the set of connections that have completed
    // the WalSubscribeRequest handshake and are receiving live WalRecord PDUs.
    pubsub_itc_fw::ExternalWalSubscriberRegistry external_wal_subscriber_registry_;
    std::unordered_set<pubsub_itc_fw::ConnectionID> wal_subscriber_conn_ids_;

    // Leader-follower state machine (slice 6).
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};

    // The leadership generation. Never assign to this directly: go through
    // set_epoch(), which also writes it to disk. A restart that forgets the
    // epoch lets this node claim a generation the venue has already used.
    int32_t epoch_{0};

    // Where the epoch outlives the process. Read once at startup, rewritten
    // whenever the epoch moves.
    fix_common::EpochStore epoch_store_;

    // Context for an arbitration request that is still outstanding, so the
    // timeout can fall back the way this particular request needs.
    //
    // The two callers face different situations. A peer heartbeat timeout means
    // the peer is believed gone, so taking leadership unopposed is right. An
    // election triggered by a peer's StatusQuery means the peer is demonstrably
    // alive and asking the same question, so taking leadership unopposed would
    // produce two leaders; that case has to be settled by a rule both sides
    // compute identically.

    // A freshly started arbiter refuses to arbitrate for a short while, because
    // an empty leadership map looks the same whether there is genuinely no
    // leader or it simply has not been told yet. It says so and asks the
    // component to retry. This counts the retries so that a silent arbiter still
    // ends in a decision rather than an indefinite wait.
    int32_t arbitration_attempts_{0};

    // True from asking the arbiter until it answers or the retries run out.
    // The peer exchange runs an election on both the query and the response, so
    // without this a single startup sends the arbiter four identical reports.
    bool arbitration_outstanding_{false};

    // Enough attempts to outlast the arbiter's learning period at the configured
    // arbitration timeout, with room to spare. Running out means no arbiter is
    // coming, not that one is still warming up.
    static constexpr int32_t max_arbitration_attempts{6};

    // Timer ids (default-constructed = not scheduled); on_timer_event compares
    // a fired timer's id against these to identify it.
    pubsub_itc_fw::TimerID wal_snapshot_timer_id_{};
    pubsub_itc_fw::TimerID peer_heartbeat_timer_id_{};
    pubsub_itc_fw::TimerID peer_heartbeat_timeout_timer_id_{};
    pubsub_itc_fw::TimerID arbiter_heartbeat_timer_id_{};
    pubsub_itc_fw::TimerID arbitration_timeout_timer_id_{};

    // WAL replication state (Slice 7).
    //
    // An ExecutionReport from the ME is held here until the follower has
    // confirmed it wrote the corresponding WAL entry.  The gateway only sees
    // the ER once the follower has durably committed it.
    struct PendingEr {
        int16_t pdu_id{};
        int64_t seq_no{};
        std::vector<uint8_t> payload; // copy of the raw encoded ER from ME
        // Whose report this is. Deliberately the identity and not a resolved destination:
        // this record exists precisely because delivery is being deferred until the
        // follower acks, and a session can reconnect during that wait -- which is the case
        // a gateway failover produces. Resolving the address when the ER was buffered would
        // send it to whichever connection was current a moment before the failover.
        fix_common::SessionIdentity identity;
        // The originating gateway's ingress stamp, carried through the WalAck wait so the
        // gateway still gets it when the ER is released. Buffering here is the live HA
        // path, so dropping it would leave the round-trip metric empty in exactly the
        // configuration the venue actually runs.
        bool has_gateway_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
        bool erase_routing_entry{false};
    };

    std::unordered_map<int64_t, PendingEr> pending_er_; // seq_no -> buffered ER
    std::unordered_set<int64_t> wal_acked_seq_nos_;     // acked but ER not yet received

    // Reusable scratch buffer for encoding the WalRecord envelope before appending it
    // to the WAL (append_envelope_to_wal). Grown to the largest envelope seen and
    // reused -- no fixed cap that could silently fail to persist, no per-record alloc.
    std::vector<uint8_t> wal_encode_buffer_;

    // Leader-follower helpers.
    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void set_epoch(int32_t new_epoch);
    bool request_arbitration();
    void send_leadership_lease();
    void resolve_with_visible_peer(int64_t peer_instance_id, int32_t peer_epoch);
    void elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_peer_heartbeat();
    void send_arbiter_heartbeat();
    void send_arbitration_report();
    void handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message);

    // Replay mode helpers.
    //
    // replay_buffer_ accumulates records during WAL replay in on_initial_event().
    // dispatch_replay_records() sends them to the ME once the ME connection is up.
    struct ReplayRecord {
        int64_t seq_no{};
        int16_t pdu_id{};
        int64_t wall_time_ns{};
        std::vector<uint8_t> payload;
    };

    std::vector<ReplayRecord> replay_buffer_;
    bool replay_me_order_ready_{false}; // outbound sequencer->ME order connection up
    bool replay_me_er_ready_{false};    // inbound ME->sequencer ER connection up
    void try_dispatch_replay();         // dispatches once both flags are set
    void dispatch_replay_records();

    // WAL storage / replication helpers (peer follower).
    //
    // WalRecord doubles as the pipeline envelope (Option B): the WAL, the follower
    // replication stream and the external-subscriber stream all carry the stamped
    // WalRecord, so leader and follower WALs stay byte-identical and every reader
    // decodes envelope-then-payload. See docs/design/fix_pdu_generation.md.
    [[nodiscard]] bool needs_wal_ack() const;
    void append_envelope_to_wal(const pubsub_itc_fw_app::WalRecord& envelope);
    void send_wal_record(const pubsub_itc_fw_app::WalRecord& envelope);
    void handle_wal_record(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_wal_ack(const pubsub_itc_fw::EventMessage& message);
    void install_peer_wal_inline_handler(const pubsub_itc_fw::ConnectionID& conn_id);
    void flush_pending_er();
    void forward_pending_er(const PendingEr& pending);

    /**
     * @brief Sends an envelope-wrapped ER to the gateway the order originated from.
     * @param[in] protocol Which client protocol (see fix_common/GatewayIds.hpp).
     * @param[in] instance Which instance of that protocol, numbered from 1.
     * @param[in] er_seq_no  Sequence number stamped on the transport header.
     * @param[in] envelope   The WalRecord-wrapped ER.
     *
     * Drops the ER with a warning when that gateway instance is not currently connected;
     * the client behind it has no route, and no other instance can serve its session
     * until session provisioning and report replay land (steps 4-6 of gateway_ha.md).
     */
    void send_er_to_origin_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope);

    // External WAL subscriber helpers (MEP primary and secondary).
    void handle_wal_subscribe_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_external_wal_ack(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void stream_wal_record_to_external_subscribers(const pubsub_itc_fw_app::WalRecord& envelope);

    // ME failover reconciliation (Slice D).
    //
    // When a promoted ME-secondary sends MePositionRequest on its order connection,
    // the sequencer walks the WAL from the ME's last-applied seq_no to the WAL head,
    // streaming each NOS/OCR to that connection, then sends MePositionAck. After the
    // ack the ME is live and new orders flow to it via me_outbound_order_conn_id_.
    //
    // me_catchup_conn_id_ tracks the connection currently in catch-up so the handler
    // can stream to a specific ConnectionID rather than the buffered replay path.
    pubsub_itc_fw::ConnectionID me_catchup_conn_id_;

    // The epoch of the most recent matching-engine RoleAnnouncement believed. An announcement
    // quoting an older epoch is from an instance whose leadership has since been superseded
    // and is refused, so a rejoining engine cannot take routing back from the one that
    // replaced it. Starts at -1 so that a first announcement at epoch 0 is accepted.
    // See design-notes-for-ha.md 11b.
    int32_t me_announced_epoch_{-1};

    // Which instance last announced leadership. Kept because the announcement can arrive
    // before this sequencer's order connection to that instance exists: a restarted engine
    // announces on its ER connection, which it opens, while the order connection is one this
    // sequencer opens to it and may not have re-established yet. Without remembering, routing
    // is left pointing at the connection to the process that just died.
    int64_t me_announced_leader_instance_{0};

    // The sequencer's own ORDER connection to each matching engine instance, so that a
    // RoleAnnouncement -- which names an instance and arrives on that instance's ER
    // connection, a different socket entirely -- can be mapped to the socket orders actually
    // travel on. Routing orders down the connection an announcement arrived on sends them the
    // wrong way, which is what the first version of this did.
    std::unordered_map<int64_t, pubsub_itc_fw::ConnectionID> me_order_conn_by_instance_;

    // Primary is always instance 1 and secondary always 2; the ids are fixed for the life of
    // a deployment and the arbiter's cold-start preference relies on it. Which of them LEADS
    // moves, and is what the announcement reports.
    static constexpr int64_t me_primary_instance_id = 1;
    static constexpr int64_t me_secondary_instance_id = 2;

    /// Handles a matching engine stating which role it holds and under which epoch.
    void handle_role_announcement(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_me_position_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void stream_wal_record_to_me(const pubsub_itc_fw::ConnectionID& conn_id, int64_t record_id, int16_t pdu_id, const uint8_t* pdu_payload, size_t pdu_size,
                                 int64_t wall_time_ns);

    // Which client session an order came from. The identity, not the address: where its
    // reports go is looked up separately, at the moment of sending, so that a session which
    // has reconnected somewhere else in the meantime is still reachable.
    struct OriginSession {
        fix_common::SessionIdentity identity;

        // Wall-clock nanoseconds at which the gateway read the order off the client socket.
        // Carried here purely so it can be stamped back onto the ER envelope: the gateway
        // measures its own round trip, and nothing in the sequencer reads this value.
        //
        // It rides with the routing data rather than in a map of its own because it has
        // exactly the same lifetime and the same key -- a second map would be a second
        // thing to insert into, erase from and rebuild on replay, with no way to notice
        // when the two drifted apart.
        //
        // has_ingress_ns false means the order arrived without one -- a WAL replay, or a
        // gateway that does not stamp. It is a separate flag rather than a zero sentinel
        // so an absent stamp cannot be mistaken for an epoch-zero one.
        bool has_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
    };

    // seq_no -> the session that placed the order.
    // Keyed by the sequence number assigned to each NOS/OCR (globally unique, unlike
    // ClOrdID which is only unique per client session). Populated on each sequenced
    // NOS/OCR; rebuilt from WAL replay on startup.
    //
    // This used to hold the originating *connection* and its reports were addressed
    // straight at it. That made a report undeliverable the moment the socket closed, and
    // undeliverable to the right member even after it came back, because the connection id
    // it named was gateway-local and renumbered on reconnect. Holding the identity and
    // resolving the address at send time is what lets a reconnect -- to the same instance
    // or to the member's backup -- inherit reports for orders it placed on the old one.
    std::unordered_map<int64_t, OriginSession> seq_no_to_session_;

    // session identity -> where that session's reports go right now.
    //
    // Maintained from the SessionBound and SessionUnbound PDUs the gateways send as
    // sessions come and go, which is the only way the sequencer can know: it listens on one
    // port and accepts, so it cannot tell instances apart from a connection alone, and a
    // member that reconnects and sends no order would otherwise never announce itself.
    //
    // An absent entry means the session is not connected anywhere. Its reports have nowhere
    // to go and are dropped, exactly as they were before -- making them replayable instead
    // is step 6, and it is deliberately not smuggled in here.
    std::unordered_map<fix_common::SessionIdentity, fix_common::SessionDestination, fix_common::SessionIdentityHash> session_destinations_;

    /**
     * @brief What the venue remembers about a session between connections.
     *
     * Sequence numbers belong to the session, not to the connection carrying it, so a
     * gateway that has just taken a session on has no way to know where it had reached.
     * The sequencer does, because every instance of every protocol binds through it -- and
     * this is the state that makes a reconnect a continuation rather than a reset.
     *
     * The numbers are *reported* by the gateway at unbind rather than counted here. They
     * have to be: the FIX outbound number counts every message sent to the member,
     * including the heartbeats and session-level rejects the sequencer never sees. So this
     * is as current as the last clean unbind, and a killed gateway leaves it behind --
     * which the member's own ResendRequest is what resolves.
     *
     * Kept separately from session_destinations_ because the lifetimes differ: a
     * destination is erased the moment a session disconnects, whereas this must outlive
     * exactly that event to be of any use.
     */
    struct SessionSequenceState {
        /// Highest position the gateway has reported. Never lowered.
        int32_t outbound_seq_num{1};

        /// Execution reports forwarded to this session since that report arrived.
        ///
        /// Each one consumed an outbound sequence number at the gateway, so this is how much
        /// the reported figure is known to be behind. Exact, because the sequencer resolves a
        /// destination for every report it sends -- and reports are the bulk of what a member
        /// is sent. What it cannot see is the admin traffic (heartbeats, rejects), which is
        /// what the allowance below covers.
        int32_t ers_since_report{0};
    };

    /// Added when resuming a session whose gateway died without reporting.
    ///
    /// Covers the admin messages the sequencer never sees. Small, because a report is only
    /// seconds old, and heartbeats are the main thing it misses. Erring high is deliberate:
    /// resuming ABOVE the true position leaves a gap the member closes with a ResendRequest,
    /// whereas resuming below sends it a sequence number lower than it expects, which FIX
    /// requires it to treat as fatal. The two errors are not symmetrical.
    static constexpr int32_t unclean_resume_admin_allowance = 64;
    std::unordered_map<fix_common::SessionIdentity, SessionSequenceState, fix_common::SessionIdentityHash> session_sequence_state_;

    void handle_session_bound(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_session_unbound(const pubsub_itc_fw::EventMessage& message);
    void handle_session_replay_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_session_sequence_update(const pubsub_itc_fw::EventMessage& message);

    /// Records that one execution report was sent to this session, so a resume after an
    /// unclean death can account for what the gateway sent since its last report.
    void note_report_forwarded(const fix_common::SessionIdentity& identity);

    /// Records a single replay may return before it truncates, when the caller names no cap.
    static constexpr int32_t default_replay_max_records = 10000;

    // Resolves where a session's reports go, or nullptr when it is not bound anywhere.
    const fix_common::SessionDestination* session_destination(const fix_common::SessionIdentity& identity) const;
};

} // namespaces
