#pragma once

#include <stdexcept> // For std::logic_error
#include <string>    // For std::string
#include <sstream>   // For std::ostringstream

// For this specific file, we will directly include the StringUtils (if leafname is moved there)
// or just use filename directly if leafname is not part of this core framework.
// For now, let's assume `Utils::leafname` is external or will be integrated.
// If it's not a common utility, it might be simplified or adapted.

namespace pubsub_itc_fw {

// Assuming a StringUtils::leafname exists, or we simplify how filename is handled.
// For now, we'll implement a simple leafname logic locally or use StringUtils if available.
// If not, a direct filename might be used, or StringUtils needs to be designed.
// Let's create a local helper for now to avoid a circular dependency if StringUtils uses PreconditionAssertion.
namespace {
    std::string get_leafname(const char* filepath) {
        std::string path_str(filepath);
        size_t pos = path_str.find_last_of("/\\");
        if (pos == std::string::npos) {
            return path_str;
        }
        return path_str.substr(pos + 1);
    }
} // namespace

/** @ingroup utilities_subsystem */

/**
 * @brief Exception class for representing a precondition assertion failure.
 *
 * This class derives from `std::logic_error` and is used to signal situations
 * where a function's precondition has been violated. It provides detailed
 * context including a message, filename, and line number where the assertion
 * was thrown, assisting in debugging.
 */
class PreconditionAssertion : public std::logic_error {
public:
    /**
     * @brief Constructs a PreconditionAssertion with a message, filename, and line number.
     * @param message The detailed error message.
     * @param filename The name of the file where the assertion was triggered.
     * @param line_number The line number in the file where the assertion was triggered.
     */
    PreconditionAssertion(const std::string& message, const char* filename, int line_number)
        : std::logic_error(assemble_assertion_message(message, filename, line_number)) {}

    /**
     * @brief Constructs a PreconditionAssertion with a C-style string message, filename, and line number.
     * @param message The detailed error message as a C-style string.
     * @param filename The name of the file where the assertion was triggered.
     * @param line_number The line number in the file where the assertion was triggered.
     */
    PreconditionAssertion(const char* message, const char* filename, int line_number)
        : std::logic_error(assemble_assertion_message(std::string(message), filename, line_number)) {}

private:
    /**
     * @brief Helper function to assemble the full assertion message string.
     * @param message The detailed error message.
     * @param filename The name of the file where the assertion was triggered.
     * @param line_number The line number in the file where the assertion was triggered.
     * @return The formatted assertion message.
     */
    static std::string assemble_assertion_message(const std::string& message, const char* filename, int line_number) {
        std::ostringstream output_stream; // Changed to snake_case
        output_stream << message << " (thrown from " << get_leafname(filename) << ":" << line_number << ")";
        return output_stream.str();
    }
};

} // namespace pubsub_itc_fw
