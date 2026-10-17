#include "router/astar.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace router {
namespace {
struct QueueNode { int id; double g, h; std::size_t order; };
struct Later {
    bool operator()(const QueueNode& a, const QueueNode& b) const {
        if (a.g + a.h != b.g + b.h) return a.g + a.h > b.g + b.h;
        if (a.h != b.h) return a.h > b.h;
        return a.order > b.order;
    }
};
} // namespace

SearchResult findPath(const GCellGrid& grid, Cell start, const std::vector<Cell>& goals) {
    SearchResult result;
    if (grid.blocked(start) || goals.empty()) return result;
    std::vector<bool> isGoal(grid.size(), false), goalLayers(grid.layerCount(), false);
    Rect bounds;
    bool hasGoal = false;
    for (const auto goal : goals) {
        if (grid.blocked(goal)) continue;
        const auto p = grid.center(goal);
        if (!hasGoal) bounds = {p.x, p.y, p.x, p.y};
        bounds.x1 = std::min(bounds.x1, p.x); bounds.x2 = std::max(bounds.x2, p.x);
        bounds.y1 = std::min(bounds.y1, p.y); bounds.y2 = std::max(bounds.y2, p.y);
        isGoal[grid.id(goal)] = true;
        goalLayers[goal.layer] = true;
        hasGoal = true;
    }
    if (!hasGoal) return result;
    const double infinity = std::numeric_limits<double>::infinity();
    const auto heuristic = [&](Cell c) {
        const auto p = grid.center(c);
        const auto dx = std::max({bounds.x1 - p.x, Coord{0}, p.x - bounds.x2});
        const auto dy = std::max({bounds.y1 - p.y, Coord{0}, p.y - bounds.y2});
        double z = infinity;
        for (int layer = 0; layer < grid.layerCount(); ++layer)
            if (goalLayers[layer]) z = std::min(z, grid.layerLowerBound(c.layer, layer));
        return static_cast<double>(dx + dy) / grid.dbu() + z;
    };
    const auto startH = heuristic(start);
    if (!std::isfinite(startH)) return result;

    // g 是已走代价，h 是剩余代价下界，队列优先取 f = g + h 最小的节点。
    std::vector<double> g(grid.size(), infinity);
    std::vector<int> parent(grid.size(), -1);
    std::priority_queue<QueueNode, std::vector<QueueNode>, Later> open;
    const auto startId = grid.id(start);
    std::size_t order = 0;
    g[startId] = 0;
    open.push({startId, 0, startH, order++});
    while (!open.empty()) {
        const auto current = open.top(); open.pop();
        if (current.g != g[current.id]) continue; // 堆里的旧条目。
        ++result.expanded;
        if (isGoal[current.id]) {
            result.found = true;
            result.cost = current.g;
            for (int at = current.id; at != -1; at = parent[at]) result.path.push_back(grid.cell(at));
            std::reverse(result.path.begin(), result.path.end());
            return result;
        }
        const auto from = grid.cell(current.id);
        for (const auto next : grid.neighbors(from)) {
            const auto nextId = grid.id(next);
            const double tentative = current.g + grid.stepCost(from, next);
            if (tentative < g[nextId]) {
                g[nextId] = tentative;
                parent[nextId] = current.id;
                open.push({nextId, tentative, heuristic(next), order++});
            }
        }
    }
    return result;
}

} // namespace router
