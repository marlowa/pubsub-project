// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixRejections.hpp"

#include <string>
#include <string_view>

namespace fix_order_gateway {

FixMessage make_reject_execution_report(std::string_view order_id, std::string_view exec_id, std::string_view cl_ord_id, std::string_view symbol,
                                        std::string_view side_str, std::string_view order_qty, std::string_view reason,
                                        pubsub_itc_fw_app::OrdRejReason ord_rej_reason) {
    FixMessage er;
    er.set(Tag::MsgType, MsgType::ExecutionReport);
    er.set(Tag::OrderID, order_id);
    er.set(Tag::ExecID, exec_id);
    er.set(Tag::ExecType, std::string(1, '8'));  // 150 = 8 Rejected
    er.set(Tag::OrdStatus, std::string(1, '8')); //  39 = 8 Rejected
    er.set(Tag::ClOrdID, cl_ord_id);
    er.set(Tag::Symbol, symbol);
    if (!side_str.empty()) {
        er.set(Tag::Side, side_str);
    }
    if (!order_qty.empty()) {
        er.set(Tag::OrderQty, order_qty);
        er.set(Tag::LeavesQty, std::string("0"));
        er.set(Tag::CumQty, std::string("0"));
    }
    er.set(Tag::OrdRejReason, static_cast<int>(ord_rej_reason));
    er.set(Tag::Text, reason);
    return er;
}

FixMessage make_request_reject(std::string_view ref_seq_num, std::string_view ref_msg_type, int ref_tag, int session_reject_reason, std::string_view text) {
    FixMessage reject;
    reject.set(Tag::MsgType, MsgType::Reject);
    reject.set(Tag::RefSeqNum, ref_seq_num);
    reject.set(Tag::RefMsgType, ref_msg_type);
    if (ref_tag != 0) {
        reject.set(Tag::RefTagID, ref_tag);
    }
    reject.set(Tag::SessionRejectReason, session_reject_reason);
    reject.set(Tag::Text, text);
    return reject;
}

} // namespaces
