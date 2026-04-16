#include "aauto/utils/Logger.hpp"

#include <chrono>
#include <iomanip>
#include <mutex>
#include <thread>
#include <iostream>

#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace aauto {
namespace utils {

static std::mutex g_log_mutex;
static LogLevel g_min_level = LogLevel::INFO;

void SetMinLogLevel(LogLevel level) { g_min_level = level; }
LogLevel GetMinLogLevel() { return g_min_level; }

static thread_local std::string g_thread_session_tag;

void SetThreadSessionTag(const std::string& tag) { g_thread_session_tag = tag; }
const std::string& GetThreadSessionTag() { return g_thread_session_tag; }

#if !defined(__ANDROID__)
static const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "D";
        case LogLevel::INFO:  return "I";
        case LogLevel::WARN:  return "W";
        case LogLevel::ERROR: return "E";
        default: return "U";
    }
}
#endif

LogMessage::LogMessage(LogLevel level, const char* tag)
    : level_(level), tag_(tag), enabled_(level >= g_min_level) {}

LogMessage::~LogMessage() {
    if (!enabled_) return;
    const std::string& session_tag = g_thread_session_tag;
    std::lock_guard<std::mutex> lock(g_log_mutex);
#if defined(__ANDROID__)
    android_LogPriority prio;
    switch (level_) {
        case LogLevel::DEBUG: prio = ANDROID_LOG_DEBUG; break;
        case LogLevel::INFO:  prio = ANDROID_LOG_INFO;  break;
        case LogLevel::WARN:  prio = ANDROID_LOG_WARN;  break;
        case LogLevel::ERROR: prio = ANDROID_LOG_ERROR; break;
        default:              prio = ANDROID_LOG_INFO;  break;
    }
    if (session_tag.empty()) {
        __android_log_print(prio, tag_, "%s", stream_.str().c_str());
    } else {
        __android_log_print(prio, tag_, "[%s] %s", session_tag.c_str(), stream_.str().c_str());
    }
#else
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);

    std::ostringstream time_stream;
    time_stream << std::setfill(' ') << std::setw(3) << seconds.count() % 1000
                << '.' << std::setfill('0') << std::setw(6) << microseconds.count();

    uint64_t tid = 0;
#if defined(__APPLE__)
    pthread_threadid_np(NULL, &tid);
#elif defined(__linux__)
    tid = static_cast<uint64_t>(syscall(SYS_gettid));
#else
    std::ostringstream tid_stream;
    tid_stream << std::this_thread::get_id();
    tid_stream >> std::hex >> tid;
    if (tid_stream.fail()) {
        tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    }
#endif

    std::ostringstream tid_hex;
    tid_hex << std::setfill('0') << std::setw(8) << std::hex << tid;

    std::cout << "[" << time_stream.str() << "] "
              << "[" << tid_hex.str() << "] "
              << "[" << LevelToString(level_) << "] ";
    if (!session_tag.empty()) {
        std::cout << "[" << session_tag << "] ";
    }
    std::cout << "[" << tag_ << "] "
              << stream_.str() << std::endl;
#endif
}

}  // namespace utils
}  // namespace aauto
