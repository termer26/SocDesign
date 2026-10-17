#pragma once
#include "router/gcell_grid.hpp"
namespace router {
std::string makeRoutedDef(const Design& design, const GCellGrid& grid,
                          const std::vector<RoutedNet>& routes);
void writeRoutedDef(const std::string& path, const Design& design,
                    const GCellGrid& grid, const std::vector<RoutedNet>& routes);
} // namespace router
