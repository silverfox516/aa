#pragma once

#include <iostream>
#include <sstream>
#include <string>

namespace aauto {
namespace utils {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

void SetMinLogLevel(LogLevel level);
LogLevel GetMinLogLevel();

// Per-thread session tag. Session worker threads call SetThreadSessionTag()
// at entry (and again after a tag update) so every log line emitted from
// that thread carries the owning session's identifier. Empty disables.
void SetThreadSessionTag(const std::string& tag);
const std::string& GetThreadSessionTag();

class LogMessage {
public:
    LogMessage(LogLevel level, const char* tag);
    ~LogMessage();

    template <typename T>
    LogMessage& operator<<(const T& value) {
        if (enabled_) stream_ << value;
        return *this;
    }

private:
    LogLevel level_;
    const char* tag_;
    bool enabled_;
    std::ostringstream stream_;
};

}  // namespace utils
}  // namespace aauto

// Convenient Macros for Logging
#ifndef LOG_TAG
#define LOG_TAG "Unknown"
#endif

#define AA_LOG_D() aauto::utils::LogMessage(aauto::utils::LogLevel::DEBUG, LOG_TAG)
#define AA_LOG_I() aauto::utils::LogMessage(aauto::utils::LogLevel::INFO, LOG_TAG)
#define AA_LOG_W() aauto::utils::LogMessage(aauto::utils::LogLevel::WARN, LOG_TAG)
#define AA_LOG_E() aauto::utils::LogMessage(aauto::utils::LogLevel::ERROR, LOG_TAG)

