#pragma once

#include <cstring>
#include <string_view>

namespace pubsub_itc_fw {

class LoggerUtils {
  public:
    static const char* leafname(const char* filename) {
        auto slen = strlen(filename);
        const char* ptr = filename + slen;
        while (ptr != filename && *(ptr - 1) != '/') {
            --ptr;
        }
        return ptr;
    }

    static std::string_view function_name(const char* function_signature) {
        std::string_view retval = function_signature;
        auto pos = retval.find("(");
        if (pos != std::string::npos) {
            retval = retval.substr(0, pos);
            pos = retval.rfind(" ");
            if (pos != std::string::npos) {
                retval = retval.substr(pos + 1);
            }
        }
        return retval;
    }
};

} // namespace pubsub_itc_fw
