#include "router/net_router.hpp"
#include "router/astar.hpp"
#include "router/pin_resolver.hpp"
#include <iostream>
#include <algorithm>
#include <queue>
#include <stdexcept>

namespace router {

Terminal chooseTerminal(const PinLocation& pin, const GCellGrid& grid, const std::vector<bool>& allowedLayers) {
    const auto allowed = [&](int layer) {
        return layer >= 0 && (allowedLayers.empty() || allowedLayers.at(layer));
    };
    // 多个矩形都是合法候选；先选中心落在可用 GCELL 的那个。
    for (const auto& shape : pin.shapes) {
        const auto layer = grid.layerIndex(shape.layer);
        const auto& r = shape.rect;
        const Point p{r.x1 + (r.x2 - r.x1) / 2, r.y1 + (r.y2 - r.y1) / 2};
        if (!allowed(layer) || !grid.contains(p)) continue;
        const auto cell = grid.locate(p, layer);
        if (!grid.blocked(cell) && !grid.pointBlocked(p, layer)) return {p, cell, {layer}};
    }
    // pin 所在格子的中心可能被宏单元遮挡，但真实 pin 本身不在障碍内。
    // 在 pin 坐标处按 LEF VIA 图换层，找同一 XY 格子中可用的金属层。
    // 不直接解除障碍，以免后续网络穿过它。
    for (const auto& shape : pin.shapes) {
        const auto source = grid.layerIndex(shape.layer);
        const auto& r = shape.rect;
        const Point p{r.x1 + (r.x2 - r.x1) / 2, r.y1 + (r.y2 - r.y1) / 2};
        if (!allowed(source) || !grid.contains(p) || grid.pointBlocked(p, source)) continue;
        std::vector<int> parent(grid.layerCount(), -1);
        std::queue<int> open;
        parent[source] = source;
        open.push(source);
        while (!open.empty()) {
            const auto layer = open.front(); open.pop();
            const auto cell = grid.locate(p, layer);
            if (!grid.blocked(cell)) {
                std::vector<int> access;
                for (int at = layer;; at = parent[at]) {
                    access.push_back(at);
                    if (at == source) break;
                }
                std::reverse(access.begin(), access.end());
                return {p, cell, access};
            }
            for (const auto& via : grid.viaLinks(layer))
                if (allowed(via.layer) && parent[via.layer] == -1 && !grid.pointBlocked(p, via.layer)) {
                    parent[via.layer] = layer;
                    open.push(via.layer);
                }
        }
    }
    throw std::runtime_error("No usable pin rectangle: " + pin.instance + "/" + pin.pin);
}

RoutedNet routeNet(const Net& net, const PinDatabase& pins, GCellGrid& grid) {
    RoutedNet result;
    if (net.use == "POWER" || net.use == "GROUND" || net.connections.size() < 2) {
        result.skipped = true;
        return result;
    }
    try {
        for (const auto& connection : net.connections)
            result.terminals.push_back(chooseTerminal(lookupPin(pins, connection), grid));
        std::vector<Cell> tree{result.terminals.front().cell};
        std::vector<bool> inTree(grid.size(), false);
        inTree[grid.id(tree.front())] = true;
        // 每个新 pin 向已有树搜索，遇到任意树节点即可停止。
        // 顺序是 DEF 连接列表顺序；这是贪心算法，不保证最优 Steiner 树。
        for (std::size_t i = 1; i < result.terminals.size(); ++i) {
            const auto search = findPath(grid, result.terminals[i].cell, tree);
            result.expanded += search.expanded;
            if (!search.found) {
                result.error = "A* cannot connect terminal " + std::to_string(i);
                result.branches.clear(); // 失败的 net 不提交资源，也不输出半条线。
                return result;
            }
            result.branches.push_back(search.path);
            for (const auto cell : search.path) {
                const auto id = grid.id(cell);
                if (!inTree[id]) { inTree[id] = true; tree.push_back(cell); }
            }
        }
        grid.commit(result.branches);
        result.success = true;
    } catch (const std::exception& e) {
        result.error = e.what();
        result.branches.clear();
    }
    return result;
}

std::vector<RoutedNet> routeNets(const Design& design, const PinDatabase& pins, GCellGrid& grid) {
    std::vector<RoutedNet> result;
    result.reserve(design.nets.size());
    for (const auto& net : design.nets) {
        auto routed = routeNet(net, pins, grid);
        if (!routed.success && !routed.skipped)
            std::cerr << "[unrouted] " << net.name << ": " << routed.error << '\n';
        result.push_back(routed);
    }
    return result;
}

} // namespace router
