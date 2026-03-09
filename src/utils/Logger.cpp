#include "aauto/utils/Logger.hpp"

#include <chrono>
#include <iomanip>
#include <mutex>
#include <thread>
#include <iostream>

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace aauto {
namespace utils {

static std::mutex g_log_mutex;

static const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "D";
        case LogLevel::INFO:  return "I";
        case LogLevel::WARN:  return "W";
        case LogLevel::ERROR: return "E";
        default: return "U";
    }
}

LogMessage::LogMessage(LogLevel level, const char* tag)
    : level_(level), tag_(tag) {}

LogMessage::~LogMessage() {
    auto now = std::chrono::system_clock::now();
    // Get time in seconds and microseconds
    auto duration = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);

    std::ostringstream time_stream;
    time_stream << std::setfill(' ') << std::setw(3) << seconds.count() % 1000 
                << '.' << std::setfill('0') << std::setw(6) << microseconds.count();

    // Extract thread id as hex
    uint64_t tid = 0;
#if defined(__APPLE__)
    pthread_threadid_np(NULL, &tid);
#elif defined(__linux__)
    tid = static_cast<uint64_t>(syscall(SYS_gettid));
#else
    std::ostringstream tid_stream;
    tid_stream << std::this_thread::get_id();
    // extract numerical value if possible or just use string hash
    tid_stream >> std::hex >> tid;
    if (tid_stream.fail()) {
        tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    }
#endif

    std::ostringstream tid_hex;
    tid_hex << std::setfill('0') << std::setw(8) << std::hex << tid;

    // 포맷: [시간] [TID] [레벨] [태그] 메시지 (libusb 스타일과 유사하게)
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "[" << time_stream.str() << "] "
              << "[" << tid_hex.str() << "] "
              << "[" << LevelToString(level_) << "] "
              << "[" << tag_ << "] "
              << stream_.str() << std::endl;
}

}  // namespace utils
}  // namespace aauto
