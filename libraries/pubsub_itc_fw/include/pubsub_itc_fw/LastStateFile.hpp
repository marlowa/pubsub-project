#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pubsub_itc_fw {

/**
 * @brief A component's last trading state: a small text file, in the venue's run directory, saying the trading day and
 *        the latest phase the component has seen (docs/venue/start_and_end_of_day.md, section 4.2, decision 5).
 *
 * The file is one line, `trading_day=20261116 EndOfDayEnd`, the trading day first as in the venue's other small files
 * (TradingDayOfFile.hpp), then the phase's name. It is written whenever the phase changes, to a temporary file that is
 * flushed and then renamed over the real one, with the directory flushed after, so that a reader sees the previous
 * line or the new one, never part of one, whatever moment the process dies at.
 *
 * It outlives the day: it is not archived at the end of day, and the next day's first write replaces it. A component
 * reads it first when it starts: if it says the end-of-day end of the day the component was started for, the
 * component has no work to do, and exits at once for the end of day.
 *
 * The phase is held as its name, so that this class needs nothing of the messages that define the phases; the
 * component, which knows them, writes and compares the names.
 */
class LastStateFile {
  public:
    /// What the file says.
    struct Recorded {
        int32_t trading_day{0};
        std::string phase;
    };

    /// The file at @p path, for a component started for @p trading_day. An empty path means the component keeps no such
    /// file, and record() does nothing.
    LastStateFile(std::string path, int32_t trading_day);

    /**
     * @brief Records that the component has seen @p phase, the phase's name.
     *
     * Throws PubSubItcException if the file cannot be written: a component that cannot record its state cannot later
     * be known to have finished the day.
     */
    void record(std::string_view phase) const;

    /// What the file at @p path says; nothing when there is no file, or it does not hold a trading day and a phase.
    [[nodiscard]] static std::optional<Recorded> read(const std::string& path);

    /// Whether the file says that @p phase of the component's own trading day has been seen.
    [[nodiscard]] bool says(std::string_view phase) const;

    [[nodiscard]] const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
    std::string temp_path_;
    int32_t trading_day_;
};

} // namespaces
