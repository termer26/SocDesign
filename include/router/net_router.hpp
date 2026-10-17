#pragma once
#include "router/gcell_grid.hpp"
namespace router {
Terminal chooseTerminal(const PinLocation& pin, const GCellGrid& grid,
                        const std::vector<bool>& allowedLayers = {});
RoutedNet routeNet(const Net& net, const PinDatabase& pins, GCellGrid& grid);
std::vector<RoutedNet> routeNets(const Design& design, const PinDatabase& pins, GCellGrid& grid);
} // namespace router
