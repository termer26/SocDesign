#pragma once
#include "router/types.hpp"
namespace router {
Design parseDef(const std::string& path);
Design parseDefText(const std::string& text);
} // namespace router
