#pragma once

#include <cstdint>
#include <cstddef>

#include <pubsub_itc_fw/LeaderFollowerProtocol.hpp>

namespace pubsub_itc_fw {

/*
    Decoded message wrapper.

    This is the output of the protocol decoder. It contains:
    - the message type (discriminant)
    - a union holding the concrete record
*/

struct LeaderFollowerDecodedMessage {
    LeaderFollowerMessageType type;

    union {
        LeaderFollowerHelloRecord hello;
        LeaderFollowerLeaderRecord leader;
        LeaderFollowerDrStatusQuery dr_query;
        LeaderFollowerDrStatusReport dr_report;
    } body;
};

/*
    Decoder result.

    The decoder returns:
    - true  => decode succeeded, 'out' is valid
    - false => decode failed (bad magic, bad version, bad size, etc.)
*/

bool decode_leader_follower_message(
    const std::uint8_t* buffer,
    std::size_t size,
    LeaderFollowerDecodedMessage& out);

} // namespace pubsub_itc_fw
