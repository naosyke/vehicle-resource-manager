// Minimal line-oriented logging: "HH:MM:SS.mmm LEVEL [component] message".
#pragma once

#include <string_view>

namespace vrm::log {

void info(std::string_view component, std::string_view message);
void warn(std::string_view component, std::string_view message);
void error(std::string_view component, std::string_view message);

}  // namespace vrm::log
