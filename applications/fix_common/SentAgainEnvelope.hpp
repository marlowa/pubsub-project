#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <leader_follower.hpp>

namespace fix_common {

/**
 * @brief The envelope a gateway sends when it sends a command again after a change of sequencer
 *        leader: the command exactly as it was first sent, marked as sent again.
 *
 * @p kept is a copy the gateway kept in its `UnansweredCommandStore`, decoded. The envelope returned
 * borrows its strings and payload, so it is valid only while the bytes @p kept was decoded from are.
 * See docs/availability/commands_during_a_change_of_leader.md, section 3.2.
 */
[[nodiscard]] inline pubsub_itc_fw_app::WalRecord envelope_to_send_again(const pubsub_itc_fw_app::WalRecordView& kept) {
    pubsub_itc_fw_app::WalRecord again{};
    again.pdu_id = kept.pdu_id;
    again.payload = kept.payload;
    again.has_gateway_session_conn_id = kept.has_gateway_session_conn_id;
    again.gateway_session_conn_id = kept.gateway_session_conn_id;
    again.has_sender_comp_id = kept.has_sender_comp_id;
    again.sender_comp_id = kept.sender_comp_id;
    again.has_origin_gateway_id = kept.has_origin_gateway_id;
    again.origin_gateway_id = kept.origin_gateway_id;
    again.has_gateway_instance_id = kept.has_gateway_instance_id;
    again.gateway_instance_id = kept.gateway_instance_id;
    // The time the gateway first received the command, which bounds how far back the new leader
    // looks for it in its log.
    again.has_gateway_ingress_ns = kept.has_gateway_ingress_ns;
    again.gateway_ingress_ns = kept.gateway_ingress_ns;
    again.has_cl_ord_id = kept.has_cl_ord_id;
    again.cl_ord_id = kept.cl_ord_id;
    again.has_sent_again = true;
    again.sent_again = true;
    return again;
}

} // namespaces
