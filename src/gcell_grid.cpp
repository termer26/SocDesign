#include "router/gcell_grid.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace router {
namespace {

std::vector<Coord> gridLines(const Design& design, char axis, Coord fallbackStep) {
    const auto low = axis == 'X' ? design.die.x1 : design.die.y1;
    const auto high = axis == 'X' ? design.die.x2 : design.die.y2;
    std::vector<Coord> lines{low, high};
    bool found = false;
    for (const auto& pattern : design.gridPatterns) {
        if (pattern.axis != axis) continue;
        found = true;
        for (int i = 0; i < pattern.count; ++i) {
            const long double v = static_cast<long double>(pattern.start)
                                  + static_cast<long double>(i) * pattern.step;
            if (v < std::numeric_limits<Coord>::min() || v > std::numeric_limits<Coord>::max())
                throw std::runtime_error("GCELLGRID coordinate overflow");
            const auto value = static_cast<Coord>(v);
            if (value > low && value < high) lines.push_back(value);
        }
    }
    if (!found) {
        if (fallbackStep <= 0)
            throw std::runtime_error(std::string("Missing GCELLGRID ") + axis + "; use --gcell-step DBU");
        if ((high - low) / fallbackStep > 1000000)
            throw std::runtime_error("Fallback GCELL grid is too large");
        for (Coord value = low; high - value > fallbackStep;) {
            value += fallbackStep;
            lines.push_back(value);
        }
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}
bool inside(Point p, Rect r) {
    return p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2;
}
bool intersects(Point a, Point b, Rect r) {
    if (a.y == b.y)
        return a.y >= r.y1 && a.y <= r.y2
               && std::max(a.x, b.x) >= r.x1 && std::min(a.x, b.x) <= r.x2;
    return a.x >= r.x1 && a.x <= r.x2
           && std::max(a.y, b.y) >= r.y1 && std::min(a.y, b.y) <= r.y2;
}
} // namespace

GCellGrid::GCellGrid(const Design& design, const Library& library, GridOptions options)
    : options_(options), dbu_(design.dbuPerMicron) {
    if (dbu_ <= 0 || options.capacity <= 0 || !std::isfinite(options.viaCost)
        || options.viaCost <= 0 || !std::isfinite(options.congestionWeight)
        || options.congestionWeight < 0)
        throw std::runtime_error("Invalid grid units/capacity/costs");
    // 限制极端输入，确保中心点、曼哈顿距离的整数运算不会溢出。
    for (const auto value : {design.die.x1, design.die.y1, design.die.x2, design.die.y2})
        if (value < -1000000000000LL || value > 1000000000000LL)
            throw std::runtime_error("DIEAREA exceeds supported coordinate range");
    xs_ = gridLines(design, 'X', options.fallbackStep);
    ys_ = gridLines(design, 'Y', options.fallbackStep);
    for (const auto& layer : library.layers)
        if (layer.type == "ROUTING") layers_.push_back(layer);
    if (layers_.empty()) throw std::runtime_error("LEF has no ROUTING layers");
    const auto total = static_cast<std::int64_t>(width()) * height() * layerCount();
    if (options.maxNodes <= 0 || options.denseMemoryMiB <= 0)
        throw std::runtime_error("Grid node and dense-memory budgets must be positive");
    if (total <= 0 || total > std::numeric_limits<int>::max() || total > options.maxNodes)
        throw std::runtime_error("Grid has " + std::to_string(total)
                                + " nodes; configured limit is " + std::to_string(options.maxNodes));
    // Fixed search/resource arrays plus the peak XY thermal aggregation buffers.
    // Paths, the heap and parsed input require additional RAM.
    const long double denseBytes = static_cast<long double>(total) * 36.125L
                                 + static_cast<long double>(width()) * height() * 44;
    if (denseBytes > static_cast<long double>(options.denseMemoryMiB) * 1048576)
        throw std::runtime_error("Grid exceeds the configured dense-array memory budget");
    blocked_.assign(static_cast<std::size_t>(total), false);
    obstacleRects_.resize(layers_.size());
    obstacleShapes_.resize(layers_.size());
    vias_.resize(layers_.size());

    // 不靠 metal1/metal2 这样的名称推断拓扑，只认 LEF VIA 的实际金属层。
    std::map<std::pair<int, int>, const Via*> selected;
    for (const auto& via : library.vias) {
        std::vector<int> metals;
        for (const auto& name : via.layers) {
            const auto index = layerIndex(name);
            if (index >= 0) metals.push_back(index);
        }
        std::sort(metals.begin(), metals.end());
        metals.erase(std::unique(metals.begin(), metals.end()), metals.end());
        if (metals.size() != 2) continue;
        const auto key = std::make_pair(metals[0], metals[1]);
        const auto it = selected.find(key);
        if (it == selected.end() || (via.isDefault && !it->second->isDefault)) selected[key] = &via;
    }
    for (const auto& item : selected) {
        const auto a = item.first.first, b = item.first.second;
        vias_[a].push_back({b, item.second->name});
        vias_[b].push_back({a, item.second->name});
    }
    // 层间最短代价用于 A* 的可采纳启发函数；无通路则为无穷大。
    const double infinity = std::numeric_limits<double>::infinity();
    layerDistance_.assign(layers_.size(), std::vector<double>(layers_.size(), infinity));
    for (int a = 0; a < layerCount(); ++a) {
        layerDistance_[a][a] = 0;
        for (const auto& via : vias_[a]) layerDistance_[a][via.layer] = options.viaCost;
    }
    for (int k = 0; k < layerCount(); ++k)
        for (int a = 0; a < layerCount(); ++a)
            for (int b = 0; b < layerCount(); ++b)
                layerDistance_[a][b] = std::min(layerDistance_[a][b],
                                               layerDistance_[a][k] + layerDistance_[k][b]);
}

bool GCellGrid::contains(Point p) const {
    return p.x >= xs_.front() && p.x <= xs_.back() && p.y >= ys_.front() && p.y <= ys_.back();
}
bool GCellGrid::valid(Cell c) const {
    return c.x >= 0 && c.x < width() && c.y >= 0 && c.y < height()
           && c.layer >= 0 && c.layer < layerCount();
}
Cell GCellGrid::locate(Point p, int layer) const {
    if (!contains(p) || layer < 0 || layer >= layerCount())
        throw std::runtime_error("Pin is outside DIEAREA or has an invalid layer");
    // 网格线上的点归右侧/上侧；最外边界归最后一个格子。
    const auto x = std::upper_bound(xs_.begin(), xs_.end(), p.x) - xs_.begin() - 1;
    const auto y = std::upper_bound(ys_.begin(), ys_.end(), p.y) - ys_.begin() - 1;
    return {std::min(static_cast<int>(x), width() - 1),
            std::min(static_cast<int>(y), height() - 1), layer};
}
Point GCellGrid::center(Cell c) const {
    if (!valid(c)) throw std::runtime_error("Invalid GCELL");
    return {xs_[c.x] + (xs_[c.x + 1] - xs_[c.x]) / 2,
            ys_[c.y] + (ys_[c.y + 1] - ys_[c.y]) / 2};
}
int GCellGrid::id(Cell c) const {
    if (!valid(c)) throw std::runtime_error("Invalid GCELL");
    return (c.layer * height() + c.y) * width() + c.x;
}
Cell GCellGrid::cell(int value) const {
    if (value < 0 || value >= size()) throw std::runtime_error("Invalid GCELL id");
    return {value % width(), (value / width()) % height(), value / (width() * height())};
}
int GCellGrid::layerIndex(const std::string& name) const {
    for (int i = 0; i < layerCount(); ++i) if (layers_[i].name == name) return i;
    return -1;
}
std::string GCellGrid::viaName(int from, int to) const {
    for (const auto& link : vias_.at(from)) if (link.layer == to) return link.name;
    throw std::runtime_error("No LEF VIA between routing layers");
}
std::uint64_t GCellGrid::edgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint32_t>(b);
}
void GCellGrid::addObstacles(const std::vector<PinShape>& shapes) {
    for (const auto& shape : shapes) {
        const auto z = layerIndex(shape.layer);
        if (z < 0) continue;
        obstacleRects_[z].push_back(shape.rect);
        obstacleShapes_[z].push_back(shape);
        if (shape.rect.x2 < xs_.front() || shape.rect.x1 > xs_.back()
            || shape.rect.y2 < ys_.front() || shape.rect.y1 > ys_.back()) continue;
        const auto bounds = [](const std::vector<Coord>& lines, Coord low, Coord high) {
            const int first = static_cast<int>(std::upper_bound(lines.begin(), lines.end(), low) - lines.begin());
            const int last = static_cast<int>(std::upper_bound(lines.begin(), lines.end(), high) - lines.begin());
            // Include the neighboring centers whose edges can cross a thin OBS.
            return std::make_pair(std::max(0, first - 2), std::min(static_cast<int>(lines.size()) - 1, last + 1));
        };
        const auto xb = bounds(xs_, shape.rect.x1, shape.rect.x2), yb = bounds(ys_, shape.rect.y1, shape.rect.y2);
        for (int y = yb.first; y < yb.second; ++y) {
            for (int x = xb.first; x < xb.second; ++x) {
                const Cell a{x, y, z}; const auto p = center(a);
                if (inside(p, shape.rect)) blocked_[id(a)] = true;
                for (const Cell b : {Cell{x + 1, y, z}, Cell{x, y + 1, z}})
                    if (valid(b) && intersects(p, center(b), shape.rect))
                        blockedEdges_.insert(edgeKey(id(a), id(b)));
            }
        }
    }
}
void GCellGrid::blockCell(Cell c) { blocked_.at(id(c)) = true; }
bool GCellGrid::blocked(Cell c) const { return !valid(c) || blocked_.at(id(c)); }
bool GCellGrid::pointBlocked(Point p, int layer) const {
    for (const auto& r : obstacleRects_.at(layer)) if (inside(p, r)) return true;
    return false;
}
bool GCellGrid::pointBlockedByOther(Point p, int layer, const std::string& pinOwner) const {
    for (const auto& shape : obstacleShapes_.at(layer))
        if (inside(p,shape.rect) && (pinOwner.empty() || shape.obstructionOwner!=pinOwner)) return true;
    return false;
}
bool GCellGrid::segmentBlocked(Point from, Point to, int layer) const {
    if (!contains(from) || !contains(to) || (from.x != to.x && from.y != to.y)) return true;
    for (const auto& r : obstacleRects_.at(layer)) if (intersects(from,to,r)) return true;
    return false;
}
std::vector<Cell> GCellGrid::neighbors(Cell c) const {
    std::vector<Cell> result;
    if (blocked(c)) return result;
    const auto add = [&](Cell b) {
        if (!blocked(b) && blockedEdges_.count(edgeKey(id(c), id(b))) == 0) result.push_back(b);
    };
    add({c.x + 1, c.y, c.layer}); add({c.x - 1, c.y, c.layer});
    add({c.x, c.y + 1, c.layer}); add({c.x, c.y - 1, c.layer});
    for (const auto& via : vias_[c.layer]) add({c.x, c.y, via.layer});
    return result;
}
double GCellGrid::stepCost(Cell from, Cell to) const {
    double base = options_.viaCost;
    if (from.layer == to.layer) {
        const auto a = center(from), b = center(to);
        base = static_cast<double>(std::abs(a.x - b.x) + std::abs(a.y - b.y)) / dbu_;
        const auto& direction = layers_[from.layer].direction;
        const bool horizontal = from.y == to.y;
        if ((direction == "HORIZONTAL" && !horizontal) || (direction == "VERTICAL" && horizontal))
            base *= 2.0; // 非首选方向可走，但代价更高。
    }
    const auto used = usage_.find(edgeKey(id(from), id(to)));
    const auto count = used == usage_.end() ? 0 : used->second;
    return base * (1.0 + options_.congestionWeight * count / options_.capacity);
}
double GCellGrid::layerLowerBound(int from, int to) const { return layerDistance_.at(from).at(to); }
void GCellGrid::commit(const std::vector<std::vector<Cell>>& branches) {
    std::unordered_set<std::uint64_t> edges;
    for (const auto& path : branches)
        for (std::size_t i = 1; i < path.size(); ++i)
            edges.insert(edgeKey(id(path[i - 1]), id(path[i])));
    for (const auto key : edges) ++usage_[key]; // 同一 net 的共享格边只计一次。
}
std::size_t GCellGrid::overflowEdges() const {
    std::size_t count = 0;
    for (const auto& used : usage_) if (used.second > options_.capacity) ++count;
    return count;
}

} // namespace router
