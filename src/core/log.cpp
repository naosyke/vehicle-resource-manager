#include "vrm/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace vrm::log {

namespace {

std::mutex output_mutex;

void write(const char* level, std::string_view component, std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_r(&seconds, &local);

    char time_text[16];
    std::strftime(time_text, sizeof(time_text), "%H:%M:%S", &local);

    std::lock_guard<std::mutex> lock(output_mutex);
    std::fprintf(stdout, "%s.%03d %-5s [%.*s] %.*s\n", time_text, static_cast<int>(millis), level,
                 static_cast<int>(component.size()), component.data(), static_cast<int>(message.size()),
                 message.data());
    std::fflush(stdout);
}

}  // namespace

void info(std::string_view component, std::string_view message) { write("INFO", component, message); }
void warn(std::string_view component, std::string_view message) { write("WARN", component, message); }
void error(std::string_view component, std::string_view message) { write("ERROR", component, message); }

}  // namespace vrm::log
