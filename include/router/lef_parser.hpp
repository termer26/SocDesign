#pragma once
#include "router/types.hpp"

namespace router {
Library parseLef(const std::string& path);
Library parseLefText(const std::string& text);
} // namespace router
