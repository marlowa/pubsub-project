#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

/** @ingroup allocator_subsystem */

class UseHugePagesFlag {
  public:
    enum UseHugePagesFlagTag { DoNotUseHugePages = 0, DoUseHugePages = 1 };

    explicit UseHugePagesFlag(UseHugePagesFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const UseHugePagesFlag& rhs) const {
        return value_ == rhs.value_;
    }

    [[nodiscard]] bool is_equal(const UseHugePagesFlagTag& rhs) const {
        return value_ == rhs;
    }

    [[nodiscard]] UseHugePagesFlagTag value() const {
        return value_;
    }

  private:
    UseHugePagesFlagTag value_;
};

inline bool operator==(const UseHugePagesFlag& lhs, const UseHugePagesFlag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const UseHugePagesFlag& lhs, const UseHugePagesFlag::UseHugePagesFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const UseHugePagesFlag::UseHugePagesFlagTag& lhs, const UseHugePagesFlag& rhs) {
    return rhs.is_equal(lhs);
}

} // namespaces
