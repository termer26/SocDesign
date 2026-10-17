#include "router/astar.hpp"
#include "router/def_parser.hpp"
#include "router/def_writer.hpp"
#include "router/lef_parser.hpp"
#include "router/net_router.hpp"
#include "router/pin_resolver.hpp"
#include "router/tokenizer.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>

using namespace router;
namespace {
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template <class Action>
void rejects(Action action, const std::string& message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}
std::string fixture(const std::string& name) {
    return std::string(ROUTER_SOURCE_DIR) + "/tests/fixtures/" + name;
}
Library simpleLibrary(int count = 2, bool withVias = true) {
    Library library;
    for (int z = 0; z < count; ++z)
        library.layers.push_back({"M" + std::to_string(z), "ROUTING",
                                  z % 2 ? "VERTICAL" : "HORIZONTAL", 0.1, 0.4});
    if (withVias)
        for (int z = 1; z < count; ++z)
            library.vias.push_back({"V" + std::to_string(z), true,
                                   {library.layers[z - 1].name, library.layers[z].name}});
    return library;
}
Design simpleDesign() {
    Design design;
    design.dbuPerMicron = 100;
    design.die = {100, -200, 710, 410};
    design.gridPatterns = {{'X', 100, 100, 7}, {'Y', -200, 100, 7}};
    return design;
}

// 独立 Dijkstra：h 恒为 0，用来验证 A* 的最短代价。
double dijkstra(const GCellGrid& grid, Cell start, const std::vector<Cell>& goals) {
    const auto infinity = std::numeric_limits<double>::infinity();
    if (grid.blocked(start)) return infinity;
    std::set<int> targets;
    for (auto goal : goals) if (!grid.blocked(goal)) targets.insert(grid.id(goal));
    std::vector<double> distance(grid.size(), infinity);
    using Entry = std::pair<double, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    distance[grid.id(start)] = 0;
    open.push({0, grid.id(start)});
    while (!open.empty()) {
        const auto item = open.top(); open.pop();
        if (item.first != distance[item.second]) continue;
        if (targets.count(item.second)) return item.first;
        for (const auto next : grid.neighbors(grid.cell(item.second))) {
            const auto cost = item.first + grid.stepCost(grid.cell(item.second), next);
            if (cost < distance[grid.id(next)]) {
                distance[grid.id(next)] = cost;
                open.push({cost, grid.id(next)});
            }
        }
    }
    return infinity;
}
void validPath(const GCellGrid& grid, const SearchResult& path,
                Cell start, const std::vector<Cell>& goals) {
    if (!path.found) { check(path.path.empty(), "Failed search must have no path"); return; }
    check(path.path.front() == start, "Wrong path start");
    check(std::find(goals.begin(), goals.end(), path.path.back()) != goals.end(), "Wrong path goal");
    double cost = 0;
    for (std::size_t i = 1; i < path.path.size(); ++i) {
        const auto neighbors = grid.neighbors(path.path[i - 1]);
        check(std::find(neighbors.begin(), neighbors.end(), path.path[i]) != neighbors.end(),
              "Path contains a forbidden/non-adjacent step");
        cost += grid.stepCost(path.path[i - 1], path.path[i]);
    }
    check(std::abs(path.cost - cost) < 1e-8, "Reported path cost differs from actual cost");
}
void connectedTree(const RoutedNet& route, const GCellGrid& grid) {
    check(route.success, "Expected successful net");
    std::map<int, std::vector<int>> graph;
    for (const auto& branch : route.branches) {
        for (std::size_t i = 1; i < branch.size(); ++i) {
            const auto a = grid.id(branch[i - 1]), b = grid.id(branch[i]);
            graph[a].push_back(b); graph[b].push_back(a);
        }
    }
    std::set<int> visited{grid.id(route.terminals.front().cell)};
    std::queue<int> open;
    open.push(*visited.begin());
    while (!open.empty()) {
        const auto a = open.front(); open.pop();
        for (const auto b : graph[a]) if (visited.insert(b).second) open.push(b);
    }
    for (const auto& pin : route.terminals)
        check(visited.count(grid.id(pin.cell)) != 0, "Multi-pin tree is disconnected");
}

void testOrientationsAndParser() {
    const std::vector<std::pair<std::string, Point>> expected = {
        {"N", {2, 3}}, {"S", {8, 5}}, {"E", {3, 8}}, {"W", {5, 2}},
        {"FN", {8, 3}}, {"FS", {2, 5}}, {"FE", {5, 8}}, {"FW", {3, 2}}};
    for (const auto& item : expected) {
        const auto p = placeMacroPoint({2, 3}, 10, 8, {100, 200}, item.first);
        check(p.x == 100 + item.second.x && p.y == 200 + item.second.y,
              "Incorrect orientation " + item.first);
    }
    rejects([] { rotatePoint({}, "INVALID"); }, "Invalid orientation should fail");
    const auto library = parseLef(fixture("tiny.lef"));
    const auto design = parseDef(fixture("tiny.def"));
    const auto pins = resolvePins(design, library);
    check(design.instances.size() == 8 && design.nets.size() == 3, "Wrong DEF counts");
    check(library.macros.at("TEST").pins.at("A").shapes.size() == 2, "Multiple LEF PORTs lost");
    const auto& r = pins.instances.at("u_N").at("A").shapes.front().rect;
    check(r.x1 == 2000 && r.y1 == 3000 && r.x2 == 4000 && r.y2 == 5000,
          "LEF ORIGIN/DBU conversion is incorrect");
    const auto& io = pins.ports.at("top_out").shapes.front().rect;
    check(io.x1 == 81200 && io.y1 == 64700 && io.x2 == 81400 && io.y2 == 64900,
          "DEF PORT rotation must use its origin, without macro SIZE normalization");
    check(design.nets.front().connections.size() == 4, "Route coordinates parsed as connections");
    check(design.nets.front().oldRoutes.size() == 1, "Old route span missing");
    const auto escaped = tokenize("- \\name(a+b) TEST ; # ignored\n - n-1 TEST ;");
    check(escaped[1].text == "\\name(a+b)" && escaped[5].text == "n-1", "Escaped/hyphenated names split");
    auto invalid = readText(fixture("tiny.def"));
    invalid.replace(invalid.find("COMPONENTS 8"), 12, "COMPONENTS 9");
    rejects([&] { parseDefText(invalid); }, "Header count mismatch not detected");
    invalid = readText(fixture("tiny.lef"));
    invalid.replace(invalid.find("RECT 1 1 3 3"), 12, "POLYGON 1 1 3 3");
    rejects([&] { parseLefText(invalid); }, "Unsupported polygon silently accepted");
    auto reversed = readText(fixture("tiny.lef"));
    reversed.replace(reversed.find("RECT 1 1 3 3"),12,"RECT 3 3 1 1");
    const auto normalized=parseLefText(reversed);
    check(normalized.normalizedRectangles==1,"Reversed LEF rectangle must be normalized and counted");
    const auto& canonical=normalized.macros.at("TEST").pins.at("A").shapes.front();
    check(canonical.x1==1 && canonical.y1==1 && canonical.x2==3 && canonical.y2==3,
          "LEF normalization must preserve both original corner coordinates");
    reversed.replace(reversed.find("RECT 3 3 1 1"),12,"RECT 1 1 1 3");
    rejects([&] { parseLefText(reversed); }, "Zero-area LEF rectangle must still be rejected");
    auto incomplete = design;
    incomplete.instances.at("u_N").placed = false;
    rejects([&] { resolvePins(incomplete, library); }, "Unplaced instance silently accepted");
}

void testGridAndAccess() {
    const auto library = parseLef(fixture("tiny.lef"));
    const auto design = parseDef(fixture("tiny.def"));
    GCellGrid grid(design, library);
    check(grid.width() == 11 && grid.height() == 9, "DO interpreted as cells, or patterns lost");
    check(grid.locate({-10000, -5000}, 0) == Cell{0, 0, 0}, "Negative grid origin broken");
    check(grid.locate({0, 5000}, 0) == Cell{1, 1, 0}, "Grid-line boundary ownership broken");
    check(grid.locate({90000, 85000}, 1) == Cell{10, 8, 1}, "Outer DIEAREA boundary broken");
    check(grid.xLines().back() == 90000 && grid.xLines()[10] == 84000, "Missing clipped end cell");
    for (int id = 0; id < grid.size(); ++id) check(grid.id(grid.cell(id)) == id, "Cell/id mismatch");
    auto noGrid = design;
    noGrid.gridPatterns.clear();
    rejects([&] { GCellGrid missing(noGrid, library); }, "Missing GCELL must not be guessed");
    GridOptions fallback;
    fallback.fallbackStep = 10000;
    GCellGrid generated(noGrid, library, fallback);
    check(generated.width() == 10 && generated.height() == 9, "Fallback GCELL wrong");
    GCellGrid obstacles(simpleDesign(), simpleLibrary());
    // 中心都不在障碍内，但两个中心之间的格边横穿细窄障碍。
    obstacles.addObstacles({{"M0", {195, -160, 205, -140}}});
    const auto next = obstacles.neighbors({0, 0, 0});
    check(std::find(next.begin(), next.end(), Cell{1, 0, 0}) == next.end(), "Thin obstacle ignored");
    check(!obstacles.blocked({0, 0, 0}), "Thin obstacle should not block this center");

    GCellGrid access(simpleDesign(), simpleLibrary());
    access.addObstacles({{"M0", {140, -160, 160, -140}}});
    PinLocation pin{"u", "TEST", "A", "SIGNAL", {{"M0", {109, -191, 111, -189}}}};
    const auto terminal = chooseTerminal(pin, access);
    check(terminal.cell.layer == 1 && terminal.accessLayers == std::vector<int>{0, 1},
          "Pin must change layer when only its GCELL center is blocked");
    check(access.blocked({0, 0, 0}), "Pin access must not globally remove an obstacle");
}

void testAStar() {
    auto design = simpleDesign();
    design.die = {0, 0, 500, 500};
    design.gridPatterns = {{'X', 0, 100, 6}, {'Y', 0, 100, 6}};
    auto library = simpleLibrary(1);
    library.layers[0].direction.clear();
    GCellGrid detour(design, library);
    for (int y = 0; y < 4; ++y) detour.blockCell({2, y, 0});
    const auto path = findPath(detour, {0, 0, 0}, {{4, 0, 0}});
    check(path.found && path.path.size() == 13 && path.cost == 12, "Detour shortest path wrong");
    validPath(detour, path, {0, 0, 0}, {{4, 0, 0}});
    detour.blockCell({2, 4, 0});
    check(!findPath(detour, {0, 0, 0}, {{4, 0, 0}}).found, "Sealed wall should be unreachable");
    const auto same = findPath(detour, {0, 0, 0}, {{0, 0, 0}});
    check(same.found && same.path.size() == 1 && same.cost == 0, "Coincident endpoints wrong");
    check(!findPath(detour, {0, 0, 0}, {}).found, "Empty goals should fail");
    GCellGrid disconnected(simpleDesign(), simpleLibrary(2, false));
    check(!findPath(disconnected, {0, 0, 0}, {{0, 0, 1}}).found, "Missing VIA should prevent layer change");

    std::mt19937 random(20261010);
    for (int trial = 0; trial < 100; ++trial) {
        GridOptions options;
        options.viaCost = 0.5 + (trial % 5);
        options.capacity = 2;
        GCellGrid grid(simpleDesign(), simpleLibrary(3), options);
        for (int id = 0; id < grid.size(); ++id)
            if (random() % 100 < 25) grid.blockCell(grid.cell(id));
        const auto start = grid.cell(static_cast<int>(random() % grid.size()));
        std::vector<Cell> goals;
        for (int i = 0; i < 1 + trial % 4; ++i)
            goals.push_back(grid.cell(static_cast<int>(random() % grid.size())));
        // 一部分地图预先占用格边，验证有方向/拥塞代价时仍然最短。
        if (trial % 3 == 0) {
            const auto preload = findPath(grid, grid.cell(0), {grid.cell(grid.size() - 1)});
            if (preload.found) grid.commit({preload.path});
        }
        const auto actual = findPath(grid, start, goals);
        const auto expected = dijkstra(grid, start, goals);
        check(actual.found == std::isfinite(expected), "A* reachability disagrees with Dijkstra");
        if (actual.found) check(std::abs(actual.cost - expected) < 1e-8, "A* is not shortest");
        validPath(grid, actual, start, goals);
    }
}

void testLocalObstacleScan() {
    auto design = simpleDesign();
    // Nonuniform cells, a negative origin and a narrow final cell exercise
    // the binary-search bounds independently of uniform-grid assumptions.
    design.gridPatterns.push_back({'X', 180, 1, 1});
    design.gridPatterns.push_back({'Y', -30, 1, 1});
    const auto library = simpleLibrary(1);
    const std::vector<Rect> rectangles = {
        {195, -160, 205, -140}, {99, -201, 151, -149},
        {710, -200, 800, 410}, {705, 405, 709, 409},
        {179, -400, 181, 600}, {-400, -31, 1000, -29},
        {-400, -400, 99, 99}, {99, -201, 711, 411}
    };
    for (const auto& rect : rectangles) {
        GCellGrid grid(design, library);
        grid.addObstacles({{"M0", rect}});
        const auto inside = [&](Point p) {
            return p.x >= rect.x1 && p.x <= rect.x2 && p.y >= rect.y1 && p.y <= rect.y2;
        };
        for (int id = 0; id < grid.size(); ++id) {
            const auto cell = grid.cell(id);
            const auto p = grid.center(cell);
            check(grid.blocked(cell) == inside(p), "Local OBS scan lost a blocked center");
            const auto neighbors = grid.neighbors(cell);
            for (const Cell next : {Cell{cell.x+1,cell.y,0}, Cell{cell.x-1,cell.y,0},
                                    Cell{cell.x,cell.y+1,0}, Cell{cell.x,cell.y-1,0}}) {
                if (!grid.valid(next)) continue;
                const auto q = grid.center(next);
                const bool intersects = std::min(p.x,q.x) <= rect.x2 && std::max(p.x,q.x) >= rect.x1
                    && std::min(p.y,q.y) <= rect.y2 && std::max(p.y,q.y) >= rect.y1;
                const bool expected = !inside(p) && !inside(q) && !intersects;
                check((std::find(neighbors.begin(),neighbors.end(),next) != neighbors.end()) == expected,
                      "Local OBS scan disagrees with exhaustive segment/rectangle geometry");
            }
        }
    }
}

void testNetsAndWriter() {
    const auto library = parseLef(fixture("tiny.lef"));
    const auto design = parseDef(fixture("tiny.def"));
    const auto pins = resolvePins(design, library);
    GCellGrid grid(design, library);
    const auto routes = routeNets(design, pins, grid);
    for (const auto& route : routes) connectedTree(route, grid);
    const auto output = makeRoutedDef(design, grid, routes);
    const auto rewritten = parseDefText(output);
    check(rewritten.nets.size() == design.nets.size(), "Writer lost nets");
    const auto netStart = design.source.find("NETS 3 ;");
    check(output.substr(0, netStart) == design.source.substr(0, netStart), "Non-net DEF sections changed");
    check(output.find("+ WEIGHT 3") != std::string::npos, "Net property lost");
    check(output.find("( 1000 1000 ) ( 2000 1000 )") == std::string::npos, "Old route not removed");
    check(output.find(" V12") != std::string::npos, "Layer changes need actual VIA names");
    for (std::size_t i = 0; i < rewritten.nets.size(); ++i) {
        check(rewritten.nets[i].connections.size() == design.nets[i].connections.size(), "Connections changed");
        check(rewritten.nets[i].oldRoutes.size() == 1, "Writer should emit one ROUTED clause with NEW paths");
    }
    std::vector<RoutedNet> failures(design.nets.size());
    check(makeRoutedDef(design, grid, failures) == design.source, "Failed nets must be preserved exactly");
    auto power = design.nets.front(); power.use = "POWER";
    check(routeNet(power, pins, grid).skipped, "Power net should be skipped");
    auto broken = design.nets.front(); broken.connections.back().pin = "MISSING";
    const auto failed = routeNet(broken, pins, grid);
    check(!failed.success && !failed.error.empty() && failed.branches.empty(), "Missing pin not reported");
    auto duplicate = design.nets.front(); duplicate.connections = {duplicate.connections[0], duplicate.connections[0]};
    connectedTree(routeNet(duplicate, pins, grid), grid);

    const auto sampleBase = std::string(ROUTER_SAMPLE_DIR) + "/";
    const auto sampleLef = parseLef(sampleBase + "sample001.input.lef");
    const auto sample = parseDef(sampleBase + "sample001.input.def");
    const auto samplePins = resolvePins(sample, sampleLef);
    GCellGrid sampleGrid(sample, sampleLef);
    sampleGrid.addObstacles(samplePins.obstructions);
    sampleGrid.addObstacles(sample.blockages);
    check(sampleGrid.width() == 42 && sampleGrid.height() == 42 && sampleGrid.layerCount() == 20,
          "Sample grid/layer counts wrong");
    const auto sampleRoutes = routeNets(sample, samplePins, sampleGrid);
    check(sampleRoutes.size() == 203, "Expected 203 sample nets");
    for (const auto& route : sampleRoutes) connectedTree(route, sampleGrid);
    const auto sampleOut = parseDefText(makeRoutedDef(sample, sampleGrid, sampleRoutes));
    check(sampleOut.instances.size() == 218 && sampleOut.pins.size() == 12 && sampleOut.nets.size() == 203,
          "Sample DEF round trip lost data");
}
} // namespace

int main() {
    try {
        testOrientationsAndParser(); testGridAndAccess(); testLocalObstacleScan(); testAStar(); testNetsAndWriter();
        std::cout << "PASS: " << checks << " checks; 100 randomized A*/Dijkstra cases; sample001.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
