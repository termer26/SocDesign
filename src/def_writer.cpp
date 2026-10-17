#include "router/def_writer.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace router {
namespace {

bool equal(Point a, Point b) { return a.x == b.x && a.y == b.y; }
void appendPoint(std::vector<Point>& points, Point p) {
    if (!points.empty() && equal(points.back(), p)) return;
    if (points.size() >= 2) {
        const auto a = points[points.size() - 2], b = points.back();
        if ((a.x == b.x && b.x == p.x) || (a.y == b.y && b.y == p.y)) {
            points.back() = p;
            return;
        }
    }
    points.push_back(p);
}

std::string routeText(const RoutedNet& route, const GCellGrid& grid) {
    std::ostringstream out;
    bool first = true;
    const auto emit = [&](int layer, const std::vector<Point>& points, const std::string& via) {
        if (points.empty()) return;
        out << (first ? "\n      + ROUTED " : "\n        NEW ") << grid.layer(layer).name;
        for (const auto p : points) out << " ( " << p.x << ' ' << p.y << " )";
        if (!via.empty()) out << ' ' << via;
        first = false;
    };
    // 接入短线：pin 中心 -> 同一 GCELL 的中心，不能只连接格子而遗漏真实 pin。
    for (const auto& terminal : route.terminals) {
        for (std::size_t i = 1; i < terminal.accessLayers.size(); ++i)
            emit(terminal.accessLayers[i - 1], {terminal.point},
                 grid.viaName(terminal.accessLayers[i - 1], terminal.accessLayers[i]));
        const auto center = grid.center(terminal.cell);
        std::vector<Point> points;
        appendPoint(points, terminal.point);
        appendPoint(points, {center.x, terminal.point.y});
        appendPoint(points, center);
        if (points.size() > 1) emit(terminal.cell.layer, points, "");
    }
    // 每条分支分成同层折线与过孔。同层共线点压缩，换层使用真实 LEF VIA。
    for (const auto& path : route.branches) {
        if (path.empty()) continue;
        int layer = path.front().layer;
        std::vector<Point> points{grid.center(path.front())};
        for (std::size_t i = 1; i < path.size(); ++i) {
            if (path[i].layer == layer) appendPoint(points, grid.center(path[i]));
            else {
                emit(layer, points, grid.viaName(layer, path[i].layer));
                layer = path[i].layer;
                points = {grid.center(path[i])};
            }
        }
        if (points.size() > 1) emit(layer, points, "");
    }
    if (first && !route.terminals.empty())
        emit(route.terminals.front().cell.layer, {route.terminals.front().point}, "");
    return out.str();
}

std::string updatedNet(const Design& design, const Net& net,
                        const RoutedNet& route, const GCellGrid& grid) {
    std::string text;
    auto position = net.source.begin;
    for (const auto span : net.oldRoutes) {
        text.append(design.source, position, span.begin - position);
        position = span.end;
    }
    // source.end 包含原来的分号，替换时把新路径放在分号之前。
    text.append(design.source, position, net.source.end - position - 1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n'
                            || text.back() == '\r' || text.back() == '\t')) text.pop_back();
    return text + routeText(route, grid) + " ;";
}
} // namespace

std::string makeRoutedDef(const Design& design, const GCellGrid& grid,
                          const std::vector<RoutedNet>& routes) {
    if (routes.size() != design.nets.size()) throw std::runtime_error("Route count mismatch");
    std::string out;
    std::size_t position = 0;
    for (std::size_t i = 0; i < routes.size(); ++i) {
        if (!routes[i].success) continue; // 失败/跳过时保留原 net，包括旧布线。
        const auto& net = design.nets[i];
        out.append(design.source, position, net.source.begin - position);
        out += updatedNet(design, net, routes[i], grid);
        position = net.source.end;
    }
    out.append(design.source, position, std::string::npos);
    return out;
}

void writeRoutedDef(const std::string& path, const Design& design,
                    const GCellGrid& grid, const std::vector<RoutedNet>& routes) {
    const auto text = makeRoutedDef(design, grid, routes);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot write DEF: " + path);
    out << text;
    if (!out) throw std::runtime_error("Failed to write DEF: " + path);
}

} // namespace router
