#pragma once

#include <string>

#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw {

class LoggingConfigurationLoader {
public:
    static RollingLogfileConfiguration load(const TomlConfiguration& toml) {
        RollingLogfileConfiguration config;
        std::string mode_str;

        // Mode is always required
        toml.get_required_except("logging.mode", mode_str);

        if (mode_str == "size") {
            config.mode = RollingLogfileConfiguration::Mode::Size;
            toml.get_required_except("logging.max_file_size", config.max_file_size);
            toml.get_required_except("logging.max_backup_files", config.max_backup_files);
        }
        else if (mode_str == "daily") {
            config.mode = RollingLogfileConfiguration::Mode::Daily;
            toml.get_required_except("logging.rotation_time", config.rotation_time);
        }
        else if (mode_str == "none") {
            config.mode = RollingLogfileConfiguration::Mode::None;
        }
        else {
            throw ConfigurationException("logging.mode must be 'size', 'daily', or 'none'");
        }

        return config;
    }
};

} // namespace pubsub_itc_fw
