#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include <fmt/format.h>

namespace pubsub_itc_fw {

/*
    Leader–Follower control-plane protocol records.

    All multi-byte fields are in network byte order.
    All records are fixed-size and validated using the magic constant.
    Reserved fields must be initialised to zero.
*/

/* protocol constants */

static constexpr std::uint32_t leader_follower_magic = 0xDEADBEEF;
static constexpr std::uint16_t leader_follower_version = 1;

/* message type value type */

class LeaderFollowerMessageType {
public:
    enum Tag {
        HelloRecord,
        LeaderRecord,
        DrStatusQuery,
        DrStatusReport
    };

public:
    constexpr explicit LeaderFollowerMessageType(Tag tag)
        : tag_{tag}
    {
    }

    Tag as_tag() const
    {
        return tag_;
    }

    bool is_equal(const LeaderFollowerMessageType& rhs) const
    {
        return tag_ == rhs.tag_;
    }

    [[nodiscard]] std::string as_string() const
    {
        if (tag_ == HelloRecord) return "HelloRecord";
        if (tag_ == LeaderRecord) return "LeaderRecord";
        if (tag_ == DrStatusQuery) return "DrStatusQuery";
        if (tag_ == DrStatusReport) return "DrStatusReport";
        return fmt::format("unknown ({})", static_cast<int>(tag_));
    }

private:
    Tag tag_{HelloRecord};
};

inline bool operator==(const LeaderFollowerMessageType& lhs, const LeaderFollowerMessageType& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator!=(const LeaderFollowerMessageType& lhs, const LeaderFollowerMessageType& rhs)
{
    return !lhs.is_equal(rhs);
}

/* protocol records */

struct LeaderFollowerHelloRecord {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t node_id;
    std::uint32_t reserved;
};

static_assert(sizeof(LeaderFollowerHelloRecord) == 12);

struct LeaderFollowerLeaderRecord {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t leader_id;
    std::uint32_t reserved;
};

static_assert(sizeof(LeaderFollowerLeaderRecord) == 12);

struct LeaderFollowerDrStatusQuery {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t message_type;
    std::uint16_t reporting_node_id;
    std::uint32_t reserved;
};

static_assert(sizeof(LeaderFollowerDrStatusQuery) == 14 + 2); // 16 bytes total

struct LeaderFollowerDrStatusReport {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t message_type;

    std::uint16_t reporting_node_id;
    std::uint16_t observed_leader_id;

    std::uint8_t main_site_reachable;   // 0 = no, 1 = yes
    std::uint8_t dr_site_reachable;     // 0 = no, 1 = yes

    std::uint8_t reserved[10];
};

static_assert(sizeof(LeaderFollowerDrStatusReport) == 24);

} // namespaces
