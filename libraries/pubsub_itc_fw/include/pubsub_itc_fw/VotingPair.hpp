#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A pair that a voter of the voting service votes in: the number its instances put on their lease messages, and the name the Quill log uses for it.
 *
 * The library knows nothing of what the pairs are. The application names them in the configuration
 * of the arbiter and the witness, and every instance of a pair must put the same number on its lease
 * messages.
 */
struct VotingPair {
    int32_t pair_id{0};
    std::string name;
};

/**
 * @brief Read one pair from the table at @p prefix: its `pair_id`, which must be positive, and its `name`, which must not be empty.
 * @param[in] toml The configuration.
 * @param[in] prefix The table, for example "own_pair" or "voting_pair[0]".
 * @param[in] loader_name The loader's name, at the start of any message.
 * Throws ConfigurationException when either is missing or not allowed.
 */
inline VotingPair load_voting_pair(const TomlConfiguration& toml, const std::string& prefix, const std::string& loader_name) {
    VotingPair pair;
    toml.get_required_except(prefix + ".pair_id", pair.pair_id);
    toml.get_required_except(prefix + ".name", pair.name);
    if (pair.pair_id <= 0) {
        throw ConfigurationException(loader_name + ": " + prefix + ".pair_id must be positive, got " + std::to_string(pair.pair_id));
    }
    if (pair.name.empty()) {
        throw ConfigurationException(loader_name + ": " + prefix + ".name must not be empty");
    }
    return pair;
}

/**
 * @brief Refuse a list of pairs in which a number or a name appears twice.
 * Throws ConfigurationException naming the first repeat found.
 */
inline void refuse_repeated_voting_pairs(const std::vector<VotingPair>& pairs, const std::string& loader_name) {
    for (size_t i = 0; i < pairs.size(); ++i) {
        for (size_t j = i + 1; j < pairs.size(); ++j) {
            if (pairs[i].pair_id == pairs[j].pair_id) {
                throw ConfigurationException(loader_name + ": pair_id " + std::to_string(pairs[i].pair_id) + " is given to both '" + pairs[i].name + "' and '" +
                                             pairs[j].name + "'");
            }
            if (pairs[i].name == pairs[j].name) {
                throw ConfigurationException(loader_name + ": the name '" + pairs[i].name + "' is given to two pairs");
            }
        }
    }
}

} // namespaces
