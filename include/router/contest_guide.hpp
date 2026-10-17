#pragma once
#include "router/contest_resources.hpp"

namespace router::contest {
struct Validation {
    bool passed = true;
    std::size_t checkedNets = 0, rectangles = 0;
    std::vector<std::string> errors;
};
void writeGuide(const std::string& path, const Design& design, const GCellGrid& grid,
                const Resources& resources, const std::vector<RoutedNet>& routes);
Validation validateGuide(const std::string& path, const Design& design,
                         const PinDatabase& pins, const GCellGrid& grid,
                         const std::set<std::string>& crossViaNames = {});
}
