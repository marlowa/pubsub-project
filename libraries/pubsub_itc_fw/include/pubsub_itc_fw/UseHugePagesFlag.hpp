#pragma once

namespace pubsub_itc_fw {

/** @ingroup allocator_subsystem */

class UseHugePagesFlag {
public:
    enum UseHugePagesFlagTag {
        DoNotUseHugePages = 0,
        DoUseHugePages    = 1
    };

    explicit UseHugePagesFlag(UseHugePagesFlagTag value) : value_{value} {}

    bool isEqual(const UseHugePagesFlag& rhs) const
    {
        return value_ == rhs.value_;
    }

    bool isEqual(const UseHugePagesFlagTag& rhs) const
    {
        return value_ == rhs;
    }

    UseHugePagesFlagTag value() const noexcept
    {
        return value_;
    }

private:
    UseHugePagesFlagTag value_;
};

inline bool operator==(const UseHugePagesFlag& lhs, const UseHugePagesFlag& rhs)
{
    return lhs.isEqual(rhs);
}

inline bool operator==(const UseHugePagesFlag& lhs, const UseHugePagesFlag::UseHugePagesFlagTag& rhs)
{
    return lhs.isEqual(rhs);
}

inline bool operator==(const UseHugePagesFlag::UseHugePagesFlagTag& lhs, const UseHugePagesFlag& rhs)
{
    return rhs.isEqual(lhs);
}

} // namespace pubsub_itc_fw
