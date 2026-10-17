#include "router/contest_router.hpp"
#include "router/net_router.hpp"
#include "router/pin_resolver.hpp"
#include "router/contest_topology.hpp"
#include "router/contest_tree_distance.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace router::contest {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double infinity = std::numeric_limits<double>::infinity();

class Budget {
public:
    Budget(Clock::time_point start, double seconds) : start_(start), limit_(seconds) {}
    double seconds() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }
    bool expired() const { return seconds() >= limit_; }
private:
    Clock::time_point start_;
    double limit_;
};

struct Search {
    std::vector<Cell> path;
    std::size_t expanded = 0;
    bool found = false, timedOut = false;
};
struct QueueNode { int id; double g, h; std::size_t order; };
struct Later {
    bool operator()(const QueueNode& a, const QueueNode& b) const {
        if (a.g + a.h != b.g + b.h) return a.g + a.h > b.g + b.h;
        if (a.h != b.h) return a.h > b.h;
        return a.order > b.order;
    }
};

// One allocation per solve, not one full-grid initialization per pin search.
// The tree stamp is independent of the search stamp: every added branch becomes
// a set of targets for all subsequent pins in this net.
class Workspace {
public:
    Workspace(const GCellGrid& grid, const Resources& resources, const DieTopology& topology, double astarWeight)
        : grid_(grid), resources_(resources), topology_(topology), astarWeight_(astarWeight), treeDistance_(grid), distance_(grid.size()), parent_(grid.size()),
          seen_(grid.size()), tree_(grid.size()), layerSeen_(grid.layerCount()),
          layerBound_(grid.layerCount(), infinity) {}

    void constrain(int die, int reservedLayer = -1, std::unordered_set<int> reservedXY = {}) {
        die_ = die; reservedLayer_ = reservedLayer; reservedXY_ = std::move(reservedXY);
    }

    void newTree() {
        advance(treeGeneration_, tree_);
        treeDistance_.clear();
        hasTree_ = false;
        std::fill(layerSeen_.begin(), layerSeen_.end(), false);
        std::fill(layerBound_.begin(), layerBound_.end(), infinity);
    }
    bool addTree(Cell cell) {
        const auto id = grid_.id(cell);
        if (tree_[id] == treeGeneration_) return false;
        tree_[id] = treeGeneration_;
        if (astarWeight_ > 1) treeDistance_.add(cell);
        const auto p = grid_.center(cell);
        if (!hasTree_) bounds_ = {p.x, p.y, p.x, p.y};
        bounds_.x1 = std::min(bounds_.x1, p.x); bounds_.x2 = std::max(bounds_.x2, p.x);
        bounds_.y1 = std::min(bounds_.y1, p.y); bounds_.y2 = std::max(bounds_.y2, p.y);
        hasTree_ = true;
        if (!layerSeen_[cell.layer]) {
            layerSeen_[cell.layer] = true;
            for (int z = 0; z < grid_.layerCount(); ++z)
                layerBound_[z] = std::min(layerBound_[z], grid_.layerLowerBound(z, cell.layer));
        }
        return true;
    }
    bool inTree(Cell cell) const { return tree_[grid_.id(cell)] == treeGeneration_; }

    Search findPath(Cell start, const Budget& budget) {
        Search result;
        if (budget.expired()) { result.timedOut = true; return result; }
        if (!hasTree_ || grid_.blocked(start) || !permitted(start)) return result;
        if (inTree(start)) { result.found = true; result.path.push_back(start); return result; }
        if (astarWeight_ > 1) treeDistance_.prepare();
        const auto startH = heuristic(start);
        if (!std::isfinite(startH)) return result;
        advance(searchGeneration_, seen_);
        heap_.clear();
        const auto startId = grid_.id(start);
        seen_[startId] = searchGeneration_;
        distance_[startId] = 0;
        parent_[startId] = -1;
        std::size_t order = 0, popped = 0;
        push({startId, 0, astarWeight_*startH, order++});
        while (!heap_.empty()) {
            // Count heap pops rather than only expansions, so stale entries
            // cannot defer a timeout indefinitely on a congested instance.
            if ((popped++ & 255U) == 0 && budget.expired()) {
                result.timedOut = true;
                return result;
            }
            std::pop_heap(heap_.begin(), heap_.end(), Later{});
            const auto current = heap_.back(); heap_.pop_back();
            if (current.g != distance_[current.id]) continue;
            ++result.expanded;
            if (tree_[current.id] == treeGeneration_) {
                for (int at = current.id; at != -1; at = parent_[at]) {
                    if ((result.path.size() & 511U) == 0 && budget.expired()) {
                        result.path.clear(); result.timedOut = true; return result;
                    }
                    result.path.push_back(grid_.cell(at));
                }
                std::reverse(result.path.begin(), result.path.end());
                result.found = true;
                return result;
            }
            const auto from = grid_.cell(current.id);
            for (const auto next : grid_.neighbors(from)) {
                if (!permitted(next) || !resources_.allowed(from, next)) continue;
                const auto nextId = grid_.id(next);
                const auto tentative = current.g + resources_.stepCost(from, next);
                if (seen_[nextId] != searchGeneration_ || tentative < distance_[nextId]) {
                    const auto h = heuristic(next);
                    if (!std::isfinite(h) || !std::isfinite(tentative)) continue;
                    seen_[nextId] = searchGeneration_;
                    distance_[nextId] = tentative;
                    parent_[nextId] = current.id;
                    push({nextId, tentative, astarWeight_*h, order++});
                }
            }
        }
        return result;
    }
private:
    bool permitted(Cell cell) const {
        return topology_.die(cell.layer) == die_
            && (cell.layer != reservedLayer_ || !reservedXY_.count(cell.y * grid_.width() + cell.x));
    }
    static void advance(std::uint32_t& generation, std::vector<std::uint32_t>& stamps) {
        if (++generation == 0) { std::fill(stamps.begin(), stamps.end(), 0); generation = 1; }
    }
    double heuristic(Cell cell) const {
        // Keep the default search's reproducible baseline; the large-case
        // weighted mode uses a tighter admissible bound before weighting it.
        if (astarWeight_ > 1) return treeDistance_.distance(cell);
        const auto p = grid_.center(cell);
        const auto dx = std::max({bounds_.x1 - p.x, Coord{0}, p.x - bounds_.x2});
        const auto dy = std::max({bounds_.y1 - p.y, Coord{0}, p.y - bounds_.y2});
        // Physical XY distance plus the VIA graph's shortest-path bound remains
        // admissible with nonuniform GCELLs, shortcut VIAs, and nonnegative
        // thermal/history/present-congestion penalties.
        return static_cast<double>(dx + dy) / grid_.dbu() + layerBound_[cell.layer];
    }
    void push(QueueNode node) {
        heap_.push_back(node);
        std::push_heap(heap_.begin(), heap_.end(), Later{});
    }
    const GCellGrid& grid_;
    const Resources& resources_;
    const DieTopology& topology_;
    double astarWeight_;
    TreeDistance treeDistance_;
    int die_ = 0, reservedLayer_ = -1;
    std::unordered_set<int> reservedXY_;
    std::vector<double> distance_;
    std::vector<int> parent_;
    std::vector<std::uint32_t> seen_, tree_;
    std::uint32_t searchGeneration_ = 0, treeGeneration_ = 0;
    std::vector<bool> layerSeen_;
    std::vector<double> layerBound_;
    std::vector<QueueNode> heap_;
    Rect bounds_{};
    bool hasTree_ = false;
};

bool usableTerminal(const Terminal& terminal, const GCellGrid& grid, const Resources& resources) {
    if (resources.capacities().at(grid.id(terminal.cell)) <= 0) return false;
    for (std::size_t i = 1; i < terminal.accessLayers.size(); ++i) {
        auto from = terminal.cell, to = terminal.cell;
        from.layer = terminal.accessLayers[i - 1]; to.layer = terminal.accessLayers[i];
        if (!resources.allowed(from, to)) return false;
    }
    for (std::size_t i = 1; i < terminal.accessPath.size(); ++i)
        if (!resources.allowed(terminal.accessPath[i-1],terminal.accessPath[i])) return false;
    return true;
}

std::optional<Terminal> escapeTerminal(const PinLocation& pin, const PinShape& shape, const GCellGrid& grid,
                                      const Resources& resources, const DieTopology& topology) {
    const auto source = grid.layerIndex(shape.layer);
    if (source < 0) return {};
    const Rect r{std::max(shape.rect.x1,grid.xLines().front()),std::max(shape.rect.y1,grid.yLines().front()),
                 std::min(shape.rect.x2,grid.xLines().back()),std::min(shape.rect.y2,grid.yLines().back())};
    if (r.x1 >= r.x2 || r.y1 >= r.y2) return {};
    // Try actual pin-covered cells, including pins spanning a GCell boundary.
    const auto first = grid.locate({r.x1,r.y1},source), last = grid.locate({r.x2-1,r.y2-1},source);
    for (int y = first.y; y <= last.y; ++y) for (int x = first.x; x <= last.x; ++x) {
        const Point p{(std::max(r.x1,grid.xLines()[x])+std::min(r.x2,grid.xLines()[x+1]))/2,
                      (std::max(r.y1,grid.yLines()[y])+std::min(r.y2,grid.yLines()[y+1]))/2};
        const auto usablePoint = [&](int layer) {
            if (!grid.pointBlocked(p,layer)) return true;
            // A pin's own metal is an access portal in its macro OBS. Keep
            // other macros and DEF blockages effective, and only permit vias
            // at points covered by this very pin on the intermediate layer.
            if (grid.pointBlockedByOther(p,layer,pin.instance)) return false;
            for (const auto& metal : pin.shapes)
                if (grid.layerIndex(metal.layer)==layer && p.x>=metal.rect.x1 && p.x<=metal.rect.x2
                    && p.y>=metal.rect.y1 && p.y<=metal.rect.y2) return true;
            return false;
        };
        if (!usablePoint(source)) continue;
        std::vector<int> parent(grid.layerCount(),-1), pending{source}; parent[source]=source;
        for (std::size_t n = 0; n < pending.size(); ++n) {
            const int layer = pending[n];
            std::vector<Cell> access;
            for (int at=layer;;at=parent[at]) { access.push_back({x,y,at}); if(at==source)break; }
            std::reverse(access.begin(),access.end());
            const auto attempt = [&](Cell target, Point corner) -> std::optional<Terminal> {
                if (grid.blocked(target) || grid.segmentBlocked(p,corner,layer)
                    || grid.segmentBlocked(corner,grid.center(target),layer)) return {};
                Terminal terminal{p,target,{},access};
                Cell at{x,y,layer};
                while (!(at==target)) {
                    if (at.x!=target.x) at.x += at.x<target.x ? 1 : -1;
                    else at.y += at.y<target.y ? 1 : -1;
                    terminal.accessPath.push_back(at);
                }
                if (usableTerminal(terminal,grid,resources)) return terminal;
                return {};
            };
            const Cell same{x,y,layer};
            if (auto terminal=attempt(same,{p.x,grid.center(same).y})) return terminal;
            // Escape a macro boundary without ever opening blocked cells to
            // other nets. Check the physical two-segment stub, not the blocked
            // center-to-center edge that only approximates this pin's access.
            for (int radius=1;radius<=4;++radius)
                for (const auto& direction : {std::pair<int,int>{0,-1},{-1,0},{0,1},{1,0}}) {
                    const Cell target{x+radius*direction.first,y+radius*direction.second,layer};
                    if (!grid.valid(target)) continue;
                    const auto center=grid.center(target);
                    const Point corner=direction.first ? Point{center.x,p.y} : Point{p.x,center.y};
                    if (auto terminal=attempt(target,corner)) return terminal;
                }
            for (const auto& via : grid.viaLinks(layer))
                if (parent[via.layer]<0 && topology.die(via.layer)==topology.die(source)
                    && usablePoint(via.layer)) {
                    parent[via.layer]=layer; pending.push_back(via.layer);
                }
        }
    }
    return {};
}

Terminal terminalFor(const PinLocation& pin, const GCellGrid& grid, const Resources& resources,
                     const DieTopology& topology) {
    // chooseTerminal knows LEF rectangles and pin-local VIA access. If its first
    // geometrically valid rectangle has zero resource capacity, try the other
    // genuine rectangles rather than moving the terminal outside the pin.
    for (const auto& shape : pin.shapes) {
        const int layer = grid.layerIndex(shape.layer);
        if (layer < 0) continue;
        auto candidate = pin;
        candidate.shapes = {shape};
        try {
            const auto terminal = chooseTerminal(candidate, grid, topology.mask(topology.die(layer)));
            if (usableTerminal(terminal, grid, resources)) return terminal;
        } catch (const std::exception&) { /* Try the remaining pin rectangles. */ }
        if (const auto terminal=escapeTerminal(pin,shape,grid,resources,topology)) return *terminal;
    }
    throw std::runtime_error("No positive-capacity pin access: " + pin.instance + "/" + pin.pin);
}

double distanceTo(Cell a, Cell b, const GCellGrid& grid) {
    const auto p = grid.center(a), q = grid.center(b);
    return static_cast<double>(std::abs(p.x - q.x) + std::abs(p.y - q.y)) / grid.dbu()
           + grid.layerLowerBound(a.layer, b.layer);
}

RoutedNet routeTree(const std::vector<Terminal>& terminals, const GCellGrid& grid,
                   Workspace& workspace, const Budget& budget, bool& timedOut) {
    RoutedNet result;
    result.terminals = terminals;
    if (budget.expired()) {
        timedOut = true; result.error = "Time limit reached before routing net"; return result;
    }
    if (terminals.size() < 2) { result.success = true; return result; }
    workspace.newTree();
    workspace.addTree(terminals.front().cell);
    std::vector<bool> connected(terminals.size(), false);
    std::vector<double> nearest(terminals.size(), infinity);
    connected[0] = true;
    for (std::size_t i = 1; i < terminals.size(); ++i)
        nearest[i] = distanceTo(terminals[i].cell, terminals.front().cell, grid);
    for (std::size_t count = 1; count < terminals.size(); ++count) {
        if (budget.expired()) {
            timedOut = true; result.error = "Time limit reached while routing net";
            result.branches.clear(); return result;
        }
        std::size_t chosen = terminals.size();
        for (std::size_t i = 1; i < terminals.size(); ++i)
            if (!connected[i] && (chosen == terminals.size() || nearest[i] < nearest[chosen])) chosen = i;
        const auto search = workspace.findPath(terminals[chosen].cell, budget);
        result.expanded += search.expanded;
        if (!search.found) {
            timedOut = search.timedOut;
            result.error = search.timedOut ? "Time limit reached in A*" :
                "A* cannot connect terminal " + std::to_string(chosen);
            result.branches.clear(); return result;
        }
        connected[chosen] = true;
        result.branches.push_back(search.path);
        std::size_t updates = 0;
        for (const auto cell : search.path) {
            if (!workspace.addTree(cell)) continue;
            for (std::size_t i = 1; i < terminals.size(); ++i) {
                if ((++updates & 2047U) == 0 && budget.expired()) {
                    timedOut = true; result.error = "Time limit reached while growing net tree";
                    result.branches.clear(); return result;
                }
                if (!connected[i]) nearest[i] = std::min(nearest[i], distanceTo(terminals[i].cell, cell, grid));
            }
        }
    }
    result.success = true;
    return result;
}

RoutedNet routeOne(const std::vector<Terminal>& terminals, const GCellGrid& grid,
                   const Resources& resources, const DieTopology& topology,
                   Workspace& workspace, const Budget& budget, bool& timedOut) {
    std::set<int> dies;
    for (const auto& terminal : terminals) dies.insert(topology.die(terminal.cell.layer));
    if (dies.size() < 2) {
        workspace.constrain(dies.empty() ? 0 : *dies.begin());
        return routeTree(terminals, grid, workspace, budget, timedOut);
    }
    RoutedNet result;
    result.terminals = terminals;
    if (dies.size() != 2) { result.error = "Routing supports at most two dies per net"; return result; }
    int aLayer = -1, bLayer = -1;
    for (const auto& pair : topology.crossings())
        if (topology.die(pair.first) != topology.die(pair.second)
            && dies.count(topology.die(pair.first)) && dies.count(topology.die(pair.second))) {
            aLayer = pair.first; bLayer = pair.second; break;
        }
    if (aLayer < 0) { result.error = "No TSV connects the pin dies"; return result; }
    std::vector<Terminal> aPins, bPins;
    std::unordered_set<int> mandatoryA, mandatoryB;
    std::vector<int> xs, ys;
    std::vector<Coord> pinXs, pinYs;
    for (const auto& terminal : terminals) {
        const bool first = topology.die(terminal.cell.layer) == topology.die(aLayer);
        (first ? aPins : bPins).push_back(terminal);
        const auto& layers = terminal.accessLayers;
        if (std::find(layers.begin(), layers.end(), first ? aLayer : bLayer) != layers.end())
            (first ? mandatoryA : mandatoryB).insert(terminal.cell.y * grid.width() + terminal.cell.x);
        for (const auto cell : terminal.accessPath)
            if (cell.layer == (first ? aLayer : bLayer))
                (first ? mandatoryA : mandatoryB).insert(cell.y*grid.width()+cell.x);
        xs.push_back(terminal.cell.x); ys.push_back(terminal.cell.y);
        pinXs.push_back(terminal.point.x); pinYs.push_back(terminal.point.y);
    }
    std::vector<int> shared;
    for (const int xy : mandatoryA) if (mandatoryB.count(xy)) shared.push_back(xy);
    if (shared.size() > 1) {
        result.error = "Pin access requires multiple TSV overlap cells; cannot construct a unique TSV";
        return result;
    }
    std::sort(xs.begin(), xs.end()); std::sort(ys.begin(), ys.end());
    const int mx = xs[xs.size()/2], my = ys[ys.size()/2];
    std::sort(pinXs.begin(), pinXs.end()); std::sort(pinYs.begin(), pinYs.end());
    const auto prefixSums = [](const std::vector<Coord>& coordinates) {
        std::vector<long double> sums(coordinates.size()+1, 0);
        for (std::size_t i = 0; i < coordinates.size(); ++i) sums[i+1] = sums[i] + coordinates[i];
        return sums;
    };
    const auto xSums = prefixSums(pinXs), ySums = prefixSums(pinYs);
    const auto distanceSum = [](const std::vector<Coord>& coordinates,
                                const std::vector<long double>& sums, Coord position) {
        const auto left = static_cast<std::size_t>(std::lower_bound(coordinates.begin(),coordinates.end(),position)
                                                  - coordinates.begin());
        return static_cast<long double>(position)*left - sums[left]
             + sums.back()-sums[left] - static_cast<long double>(position)*(coordinates.size()-left);
    };
    struct Candidate { double cost; Cell a; };
    std::vector<Candidate> candidates;
    std::set<std::pair<int,int>> visited;
    const auto add = [&](int x, int y) {
        if (budget.expired()) { timedOut = true; return; }
        const Cell a{x,y,aLayer}, b{x,y,bLayer};
        if (!grid.valid(a) || !visited.emplace(x,y).second || grid.blocked(a) || grid.blocked(b)
            || !resources.allowed(a,b)) return;
        const auto p = grid.center(a);
        // Prefix sums evaluate the same Manhattan objective in O(log pins)
        // per site; rescanning every pin at every pin site was quadratic.
        const double cost = resources.stepCost(a,b)
            + static_cast<double>(distanceSum(pinXs,xSums,p.x) + distanceSum(pinYs,ySums,p.y)) / grid.dbu();
        candidates.push_back({cost,a});
    };
    if (!shared.empty()) add(shared.front() % grid.width(), shared.front() / grid.width());
    else {
        add(mx,my);
        for (const auto& terminal : terminals) {
            add(terminal.cell.x,terminal.cell.y);
            if (timedOut) break;
        }
        // Deterministic nearby alternatives, including a wider ring if the
        // median lies inside a macro. Each candidate is independently checked.
        const int radius = std::max(grid.width(),grid.height());
        for (int r = 1; !timedOut && r <= radius && (r <= 4 || candidates.size() < 64); ++r) {
            if (budget.expired()) { timedOut = true; break; }
            for (int dx = -r; dx <= r; ++dx) { add(mx+dx,my-r); add(mx+dx,my+r); }
            for (int dy = -r+1; dy < r; ++dy) { add(mx-r,my+dy); add(mx+r,my+dy); }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::tie(a.cost,a.a.y,a.a.x) < std::tie(b.cost,b.a.y,b.a.x);
    });
    std::string lastError = "No usable TSV landing site";
    for (std::size_t i = 0; i < std::min<std::size_t>(32,candidates.size()); ++i) {
        if (budget.expired()) { timedOut = true; break; }
        const auto a = candidates[i].a;
        const Cell b{a.x,a.y,bLayer};
        const int xy = a.y*grid.width()+a.x;
        auto reserved = mandatoryB; reserved.erase(xy);
        workspace.constrain(topology.die(aLayer),aLayer,std::move(reserved));
        auto aTerminals = aPins;
        aTerminals.insert(aTerminals.begin(), {grid.center(a),a,{aLayer}});
        auto left = routeTree(aTerminals,grid,workspace,budget,timedOut);
        result.expanded += left.expanded;
        if (!left.success) { lastError = left.error; if (timedOut) break; continue; }
        std::unordered_set<int> footprint;
        for (const auto node : resources.routeNodes(left)) {
            const auto cell = grid.cell(node);
            if (cell.layer == aLayer) footprint.insert(cell.y*grid.width()+cell.x);
        }
        footprint.erase(xy);
        workspace.constrain(topology.die(bLayer),bLayer,std::move(footprint));
        auto bTerminals = bPins;
        bTerminals.insert(bTerminals.begin(), {grid.center(b),b,{bLayer}});
        auto right = routeTree(bTerminals,grid,workspace,budget,timedOut);
        result.expanded += right.expanded;
        if (!right.success) { lastError = right.error; if (timedOut) break; continue; }
        result.branches = std::move(left.branches);
        for (auto& branch : right.branches) result.branches.push_back(std::move(branch));
        result.branches.push_back({a,b});
        result.success = true;
        return result;
    }
    result.error = timedOut ? "Time limit reached while selecting TSV" : "TSV routing failed: " + lastError;
    return result;
}

double hpwl(const std::vector<Terminal>& terminals, const GCellGrid& grid) {
    if (terminals.empty()) return 0;
    auto p = terminals.front().point;
    Rect bounds{p.x, p.y, p.x, p.y};
    int low = terminals.front().cell.layer, high = low;
    for (const auto& terminal : terminals) {
        bounds.x1 = std::min(bounds.x1, terminal.point.x); bounds.x2 = std::max(bounds.x2, terminal.point.x);
        bounds.y1 = std::min(bounds.y1, terminal.point.y); bounds.y2 = std::max(bounds.y2, terminal.point.y);
        low = std::min(low, terminal.cell.layer); high = std::max(high, terminal.cell.layer);
    }
    const auto layerCost = grid.layerLowerBound(low, high);
    return static_cast<double>(bounds.x2 - bounds.x1 + bounds.y2 - bounds.y1) / grid.dbu() + layerCost;
}

std::size_t crossCount(const RoutedNet& route, const GCellGrid& grid, const Resources& resources) {
    std::size_t count = 0;
    if (route.success)
        for (const auto& edge : resources.routeEdges(route))
            if (resources.isCross(grid.cell(edge.first).layer, grid.cell(edge.second).layer)) ++count;
    return count;
}

Iteration measure(int index, const std::vector<RoutedNet>& routes, const GCellGrid& grid,
                  const Resources& resources, const Budget& budget,
                  std::size_t expanded, std::size_t rerouted) {
    Iteration result;
    result.index = index; result.expanded = expanded; result.rerouted = rerouted;
    for (const auto& route : routes) {
        if (!route.success) ++result.failed;
        else {
            result.crossVias += crossCount(route, grid, resources);
            result.cost += resources.staticCost(route);
        }
    }
    result.overflow = resources.overflow();
    result.correlation = resources.correlation();
    result.seconds = budget.seconds();
    return result;
}

bool better(const Iteration& a, const Iteration& b) {
    return std::tie(a.failed, a.overflow, a.crossVias, a.cost)
         < std::tie(b.failed, b.overflow, b.crossVias, b.cost);
}

struct Priority {
    std::size_t net = 0, rank = 0, cross = 0;
    long long overflow = 0;
    double cost = 0;
    bool failed = false;
};
} // namespace

Result route(const Design& design, const PinDatabase& pins, const GCellGrid& grid,
             Resources& resources, Options options,
             const std::function<void(const Iteration&)>& progress) {
    const auto started = Clock::now();
    if (options.iterations < 0 || options.timeLimitSeconds < 0 || !std::isfinite(options.timeLimitSeconds))
        throw std::runtime_error("Invalid contest iterations/time limit");
    if (!std::isfinite(options.astarWeight) || options.astarWeight < 1 || options.astarWeight > 3)
        throw std::runtime_error("A* weight must be in [1,3]");
    const Budget budget(started, options.timeLimitSeconds);
    const DieTopology topology(grid, resources.crossViaNames());
    Result result;
    result.routes.resize(design.nets.size());
    resources.clearUsage();
    resources.setPresentFactor(1);
    std::vector<std::vector<Terminal>> terminals(design.nets.size());
    std::vector<bool> prepared(design.nets.size(), false);
    std::vector<double> lengths(design.nets.size(), infinity);
    std::vector<std::size_t> order(design.nets.size()), rank(design.nets.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    for (auto& net : result.routes) net.error = "Not routed before time limit";
    for (std::size_t n = 0; n < design.nets.size(); ++n) {
        if (budget.expired()) { result.timeLimitReached = true; break; }
        try {
            terminals[n].reserve(design.nets[n].connections.size());
            for (const auto& connection : design.nets[n].connections) {
                if (budget.expired()) { result.timeLimitReached = true; break; }
                terminals[n].push_back(terminalFor(lookupPin(pins, connection), grid, resources, topology));
            }
            if (result.timeLimitReached) break;
            lengths[n] = hpwl(terminals[n], grid);
            prepared[n] = true;
        } catch (const std::exception& e) { result.routes[n].error = e.what(); }
    }
    if (!options.inputOrder)
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (lengths[a] != lengths[b]) return lengths[a] < lengths[b];
            if (terminals[a].size() != terminals[b].size()) return terminals[a].size() < terminals[b].size();
            return a < b;
        });
    for (std::size_t i = 0; i < order.size(); ++i) rank[order[i]] = i;

    // Start the timer before terminal preprocessing and workspace allocation.
    // A zero or exhausted budget still returns one failed entry for every net.
    std::unique_ptr<Workspace> workspace;
    if (budget.expired()) result.timeLimitReached = true;
    if (!result.timeLimitReached) workspace = std::make_unique<Workspace>(grid, resources, topology, options.astarWeight);
    std::size_t roundExpanded = 0, attempts = 0;
    double lastProgress = budget.seconds();
    if (!result.timeLimitReached)
        for (const auto n : order) {
            if (budget.expired()) { result.timeLimitReached = true; break; }
            if (!prepared[n]) continue;
            auto candidate = routeOne(terminals[n], grid, resources, topology, *workspace, budget, result.timeLimitReached);
            roundExpanded += candidate.expanded;
            ++attempts;
            resources.commit(candidate);
            result.routes[n] = std::move(candidate);
            if (options.netProgress && budget.seconds()-lastProgress >= 5) {
                lastProgress=budget.seconds();
                options.netProgress(rank[n]+1,order.size(),lastProgress);
            }
            if (result.timeLimitReached) break;
        }
    result.expanded += roundExpanded;
    auto best = measure(0, result.routes, grid, resources, budget, roundExpanded, attempts);
    best.selected = true;
    result.iterations.push_back(best);
    if (progress) progress(best);
    auto bestRoutes = result.routes;
    double presentFactor = 1;

    for (int iteration = 1; iteration <= options.iterations && !result.timeLimitReached; ++iteration) {
        if (budget.expired()) { result.timeLimitReached = true; break; }
        resources.updateHistory();
        presentFactor = std::min(1000000.0, presentFactor * 1.5);
        resources.setPresentFactor(presentFactor);
        std::vector<Priority> priorities;
        priorities.reserve(order.size());
        for (const auto n : order) {
            if (budget.expired()) { result.timeLimitReached = true; break; }
            if (!prepared[n] || terminals[n].size() < 2) continue;
            Priority priority;
            priority.net = n; priority.rank = rank[n]; priority.failed = !result.routes[n].success;
            if (!priority.failed) {
                for (const auto node : resources.routeNodes(result.routes[n]))
                    priority.overflow += std::max(0, resources.usage()[node] - resources.capacities()[node]);
                priority.cross = crossCount(result.routes[n], grid, resources);
                priority.cost = resources.staticCost(result.routes[n]);
            }
            priorities.push_back(priority);
        }
        if (result.timeLimitReached || priorities.empty()) break;
        std::stable_sort(priorities.begin(), priorities.end(), [](const Priority& a, const Priority& b) {
            if (a.failed != b.failed) return a.failed > b.failed;
            if (a.overflow != b.overflow) return a.overflow > b.overflow;
            if (a.cross != b.cross) return a.cross > b.cross;
            if (a.cost != b.cost) return a.cost > b.cost;
            return a.rank < b.rank;
        });
        roundExpanded = 0; attempts = 0;
        bool changed = false;
        for (const auto& priority : priorities) {
            if (budget.expired()) { result.timeLimitReached = true; break; }
            const auto n = priority.net;
            auto previous = std::move(result.routes[n]);
            resources.commit(previous, -1);
            auto candidate = routeOne(terminals[n], grid, resources, topology, *workspace, budget, result.timeLimitReached);
            roundExpanded += candidate.expanded;
            ++attempts;
            // A failed/expired replacement cannot destroy an existing route.
            // Successful replacements can temporarily worsen the global score:
            // negotiated congestion needs that freedom to escape local minima.
            if (!candidate.success && previous.success) result.routes[n] = std::move(previous);
            else {
                changed = changed || candidate.success != previous.success || candidate.branches != previous.branches;
                result.routes[n] = std::move(candidate);
            }
            resources.commit(result.routes[n]);
            if (result.timeLimitReached) break;
        }
        result.expanded += roundExpanded;
        auto current = measure(iteration, result.routes, grid, resources, budget, roundExpanded, attempts);
        if (better(current, best)) {
            current.selected = true;
            best = current;
            bestRoutes = result.routes;
        }
        result.iterations.push_back(current);
        if (progress) progress(current);
        if (!changed && current.failed == 0 && current.overflow == 0) break;
    }

    // A deadline in the middle of an iteration is safe: every ripped-up net
    // has already been either recommitted or rolled back before this restore.
    result.routes = std::move(bestRoutes);
    resources.clearUsage();
    for (const auto& routed : result.routes) resources.commit(routed);
    result.timeLimitReached = result.timeLimitReached || budget.expired();
    return result;
}
} // namespace router::contest
