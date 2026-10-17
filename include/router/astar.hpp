#pragma once
#include "router/gcell_grid.hpp"
namespace router {
struct SearchResult {
    bool found = false;
    double cost = 0;
    std::size_t expanded = 0;
    std::vector<Cell> path;
};
// goals 可以只有一个终点，也可以是已有布线树的所有节点。
SearchResult findPath(const GCellGrid& grid, Cell start, const std::vector<Cell>& goals);
} // namespace router
