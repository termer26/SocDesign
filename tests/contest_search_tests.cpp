#include "router/contest_router.hpp"
#include "router/contest_guide.hpp"
#include "router/contest_tree_distance.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>

using namespace router;
using namespace router::contest;
namespace {
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
Library library(int layers = 1) {
    Library result;
    for (int z = 0; z < layers; ++z)
        result.layers.push_back({"M" + std::to_string(z), "ROUTING", "", 0.1, 1.0});
    for (int z = 1; z < layers; ++z)
        result.vias.push_back({"V" + std::to_string(z), true,
                              {"M" + std::to_string(z - 1), "M" + std::to_string(z)}});
    return result;
}
Design design(int width = 7, int height = 5) {
    Design result;
    result.dbuPerMicron = 100;
    result.die = {0, 0, width * 100, height * 100};
    result.gridPatterns = {{'X', 0, 100, width + 1}, {'Y', 0, 100, height + 1}};
    return result;
}
Connection pin(PinDatabase& pins, const GCellGrid& grid, std::string name, Cell cell) {
    const auto p = grid.center(cell);
    pins.ports[name] = {"PIN", "", name, "SIGNAL",
                       {{grid.layer(cell.layer).name, {p.x - 1, p.y - 1, p.x + 1, p.y + 1}}}};
    return {"PIN", name};
}
Net net(std::string name, std::vector<Connection> connections, std::string use = "SIGNAL") {
    Net result;
    result.name = std::move(name); result.connections = std::move(connections); result.use = std::move(use);
    return result;
}
Options options(int iterations = 0) {
    Options result;
    result.iterations = iterations; result.timeLimitSeconds = 30;
    return result;
}
void connected(const RoutedNet& route, const GCellGrid& grid, const Resources& resources) {
    check(route.success && !route.skipped, "Expected every routable NETS entry to succeed");
    if (route.terminals.empty()) { check(route.branches.empty(), "Zero-pin net acquired geometry"); return; }
    std::map<int, std::vector<int>> edges;
    for (const auto& path : route.branches) {
        for (std::size_t i = 1; i < path.size(); ++i) {
            const auto neighbors = grid.neighbors(path[i - 1]);
            check(std::find(neighbors.begin(), neighbors.end(), path[i]) != neighbors.end(),
                  "Illegal grid edge in returned route");
            check(resources.allowed(path[i - 1], path[i]), "Illegal resource edge in returned route");
            const auto a = grid.id(path[i - 1]), b = grid.id(path[i]);
            edges[a].push_back(b); edges[b].push_back(a);
        }
    }
    std::set<int> visited{grid.id(route.terminals.front().cell)};
    std::queue<int> open;
    open.push(*visited.begin());
    while (!open.empty()) {
        const auto id = open.front(); open.pop();
        for (const auto next : edges[id]) if (visited.insert(next).second) open.push(next);
    }
    for (const auto& terminal : route.terminals)
        check(visited.count(grid.id(terminal.cell)) != 0, "Returned net tree is disconnected");
}
using Score = std::tuple<std::size_t, long long, std::size_t, double>;
void bestAndUsage(const Result& result, const Design& input, const GCellGrid& grid,
                  const Resources& resources, ResourceOptions resourceOptions = {}) {
    Resources reconstructed(input, grid, resourceOptions);
    std::size_t failed = 0, cross = 0;
    double cost = 0;
    for (const auto& route : result.routes) {
        reconstructed.commit(route);
        if (!route.success) { ++failed; continue; }
        cost += resources.staticCost(route);
        for (const auto& edge : resources.routeEdges(route))
            if (resources.isCross(grid.cell(edge.first).layer, grid.cell(edge.second).layer)) ++cross;
    }
    check(reconstructed.usage() == resources.usage(), "Final occupancy does not match returned best routes");
    check(reconstructed.tsvCounts() == resources.tsvCounts(), "Final cross-VIA counts do not match returned routes");
    const Score actual{failed, resources.overflow(), cross, cost};
    check(!result.iterations.empty(), "Initial iteration statistics missing");
    auto expected = Score{result.iterations[0].failed, result.iterations[0].overflow,
                          result.iterations[0].crossVias, result.iterations[0].cost};
    std::size_t expanded = 0;
    for (const auto& iteration : result.iterations) {
        expected = std::min(expected, Score{iteration.failed, iteration.overflow, iteration.crossVias, iteration.cost});
        expanded += iteration.expanded;
    }
    check(actual == expected, "Solver did not restore the lexicographically best complete snapshot");
    check(result.expanded == expanded, "Total search expansions disagree with iteration totals");
}
bool sameRoutes(const std::vector<RoutedNet>& a, const std::vector<RoutedNet>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t n = 0; n < a.size(); ++n) {
        if (a[n].success != b[n].success || a[n].error != b[n].error || a[n].branches != b[n].branches
            || a[n].expanded != b[n].expanded || a[n].terminals.size() != b[n].terminals.size()) return false;
        for (std::size_t p = 0; p < a[n].terminals.size(); ++p) {
            const auto& x = a[n].terminals[p]; const auto& y = b[n].terminals[p];
            if (!(x.cell == y.cell) || x.point.x != y.point.x || x.point.y != y.point.y
                || x.accessLayers != y.accessLayers) return false;
        }
    }
    return true;
}
double dijkstra(const GCellGrid& grid, const Resources& resources, Cell start, Cell goal) {
    const auto inf = std::numeric_limits<double>::infinity();
    std::vector<double> distances(grid.size(), inf);
    using Entry = std::pair<double, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    distances[grid.id(start)] = 0; open.push({0, grid.id(start)});
    while (!open.empty()) {
        const auto item = open.top(); open.pop();
        if (item.first != distances[item.second]) continue;
        if (item.second == grid.id(goal)) return item.first;
        const auto from = grid.cell(item.second);
        for (const auto next : grid.neighbors(from)) {
            if (!resources.allowed(from, next)) continue;
            const auto distance = item.first + resources.stepCost(from, next);
            if (distance < distances[grid.id(next)]) {
                distances[grid.id(next)] = distance;
                open.push({distance, grid.id(next)});
            }
        }
    }
    return inf;
}

void allNetTypesAndDeterminism() {
    auto input = design();
    GCellGrid grid(input, library(3));
    PinDatabase pins;
    const auto a = pin(pins, grid, "a", {0, 0, 0});
    const auto b = pin(pins, grid, "b", {6, 4, 2});
    const auto c = pin(pins, grid, "c", {3, 1, 1});
    const auto d = pin(pins, grid, "d", {1, 4, 0});
    input.nets = {net("zero", {}), net("one", {a}), net("power", {a, b}, "POWER"),
                  net("ground", {c, d}, "GROUND"), net("many", {b, a, d, c, a})};
    Resources firstResources(input, grid), secondResources(input, grid);
    const auto first = route(input, pins, grid, firstResources, options(4));
    const auto second = route(input, pins, grid, secondResources, options(4));
    check(!first.timeLimitReached, "Tiny example reached time limit");
    check(first.routes.size() == input.nets.size(), "NETS entries were omitted");
    for (const auto& routed : first.routes) connected(routed, grid, firstResources);
    check(first.routes[1].terminals.size() == 1, "Single-pin net lost its pin");
    check(sameRoutes(first.routes, second.routes), "Identical input/options produced different geometry");
    check(first.expanded == second.expanded, "Deterministic search expansion counts differ");
    bestAndUsage(first, input, grid, firstResources);
    bestAndUsage(second, input, grid, secondResources);
}

void unreachableAndDeadline() {
    auto input = design();
    GCellGrid grid(input, library());
    for (int y = 0; y < grid.height(); ++y) grid.blockCell({3, y, 0});
    PinDatabase pins;
    const auto a = pin(pins, grid, "a", {0, 2, 0});
    const auto b = pin(pins, grid, "b", {6, 2, 0});
    input.nets = {net("cut", {a, b}), net("bad_pin", {a, {"PIN", "missing"}}), net("one", {a})};
    Resources resources(input, grid);
    const auto result = route(input, pins, grid, resources, options(2));
    check(!result.routes[0].success && result.routes[0].branches.empty() && !result.routes[0].error.empty(),
          "Disconnected net was reported successful or retained a partial tree");
    check(!result.routes[1].success && result.routes[1].error.find("Missing pin") != std::string::npos,
          "Missing pin error was lost");
    check(result.routes[2].success, "One invalid net prevented an independent net from routing");
    bestAndUsage(result, input, grid, resources);
    auto expired = options(5); expired.timeLimitSeconds = 0;
    const auto timeout = route(input, pins, grid, resources, expired);
    check(timeout.timeLimitReached && timeout.expanded == 0, "Zero deadline must stop without searching");
    for (const auto& routed : timeout.routes)
        check(!routed.success && !routed.error.empty() && routed.branches.empty(), "Timeout manufactured a successful net");
    check(std::all_of(resources.usage().begin(), resources.usage().end(), [](int use) { return use == 0; }),
          "Previous solve occupancy leaked through timed-out solve");
    bestAndUsage(timeout, input, grid, resources);
}

void negotiatedCongestion() {
    auto input = design(9, 7);
    GCellGrid grid(input, library());
    PinDatabase pins;
    const auto a = pin(pins, grid, "a", {0, 3, 0});
    const auto b = pin(pins, grid, "b", {8, 3, 0});
    for (int i = 0; i < 4; ++i) input.nets.push_back(net("n" + std::to_string(i), {a, b}));
    ResourceOptions costs;
    costs.fallbackCapacity = 1; costs.presentWeight = 0.05; costs.historyWeight = 4;
    Resources initialResources(input, grid, costs), improvedResources(input, grid, costs);
    const auto initial = route(input, pins, grid, initialResources, options());
    const auto improved = route(input, pins, grid, improvedResources, options(6));
    for (const auto& routed : improved.routes) connected(routed, grid, improvedResources);
    check(improvedResources.overflow() < initialResources.overflow(),
          "History-based rip-up and reroute did not reduce the deliberately congested baseline");
    check(improved.iterations.size() > 1, "No congestion-negotiation iterations were run");
    bestAndUsage(initial, input, grid, initialResources, costs);
    bestAndUsage(improved, input, grid, improvedResources, costs);
}

void shortestPathAgainstDijkstra() {
    std::mt19937 random(20261013);
    for (int trial = 0; trial < 64; ++trial) {
        auto input = design(6, 5);
        auto tech = library(3);
        // A direct 0 -> 2 VIA prevents a layer-index heuristic from being valid.
        tech.vias.push_back({"shortcut", true, {"M0", "M2"}});
        if (trial % 2 == 0) tech.layers[1].direction = "VERTICAL";
        GridOptions gridOptions;
        gridOptions.viaCost = 0.2 + (trial % 5);
        GCellGrid grid(input, tech, gridOptions);
        const Cell goal{0, 0, trial % 3}, start{5, 4, (trial + 1) % 3};
        for (int id = 0; id < grid.size(); ++id) {
            const auto cell = grid.cell(id);
            if (!(cell == start) && !(cell == goal) && random() % 100 < 24) grid.blockCell(cell);
        }
        PinDatabase pins;
        input.nets = {net("random", {pin(pins, grid, "goal", goal), pin(pins, grid, "start", start)})};
        ResourceOptions costs;
        costs.crossVias = {"shortcut"}; costs.crossViaCost = gridOptions.viaCost + 0.5 * (trial % 4);
        Resources resources(input, grid, costs), original(input, grid, costs);
        const auto expected = dijkstra(grid, original, start, goal);
        const auto result = route(input, pins, grid, resources, options());
        const auto& routed = result.routes.front();
        check(routed.success == std::isfinite(expected), "Contest A* reachability differs from Dijkstra");
        if (routed.success) {
            connected(routed, grid, resources);
            double actual = 0;
            for (const auto& path : routed.branches)
                for (std::size_t i = 1; i < path.size(); ++i) actual += original.stepCost(path[i - 1], path[i]);
            check(std::abs(actual - expected) < 1e-8, "Contest A* did not find the minimum weighted path");
        }
        bestAndUsage(result, input, grid, resources, costs);
        auto weightedOptions=options(); weightedOptions.astarWeight=1.5;
        Resources weightedResources(input,grid,costs);
        const auto weighted=route(input,pins,grid,weightedResources,weightedOptions);
        check(weighted.routes.front().success==std::isfinite(expected),"Weighted A* changed reachability");
        if (weighted.routes.front().success) {
            connected(weighted.routes.front(),grid,weightedResources);
            double weightedCost=0;
            for (const auto& path : weighted.routes.front().branches)
                for (std::size_t i=1;i<path.size();++i) weightedCost+=original.stepCost(path[i-1],path[i]);
            check(weightedCost>=expected-1e-8 && weightedCost<=1.5*expected+1e-8,
                  "Weighted A* violates its bound against independent Dijkstra");
        }
    }
}

void treeDistanceAgainstExhaustiveScan() {
    auto input=design(31,23); auto tech=library(4);
    tech.vias.push_back({"shortcut",true,{"M0","M3"}});
    GridOptions config; config.viaCost=0.35;
    GCellGrid grid(input,tech,config);
    TreeDistance index(grid);
    std::vector<Cell> targets;
    std::mt19937 random(20261015);
    for (int generation=0;generation<3;++generation) {
        index.clear(); targets.clear();
        check(!std::isfinite(index.distance({0,0,0})),"Empty target tree must have infinite distance");
        for (int added=0;added<310;++added) {
            const Cell target{static_cast<int>(random()%31),static_cast<int>(random()%23),static_cast<int>(random()%4)};
            targets.push_back(target); index.add(target);
            // Exercises a built tree, unindexed additions and batch rebuilds.
            if (added%3==0) index.prepare();
            const Cell query{static_cast<int>(random()%31),static_cast<int>(random()%23),static_cast<int>(random()%4)};
            const auto p=grid.center(query);
            double expected=std::numeric_limits<double>::infinity();
            for (const auto cell:targets) {
                const auto q=grid.center(cell);
                expected=std::min(expected,static_cast<double>(std::abs(p.x-q.x)+std::abs(p.y-q.y))/grid.dbu()
                    +grid.layerLowerBound(query.layer,cell.layer));
            }
            check(std::abs(index.distance(query)-expected)<1e-10,"Tree distance differs from exhaustive target scan");
            check(index.distance(target)==0,"New target was omitted from distance index");
        }
    }
}

void uniqueTsvAndDieRestriction(double weight=1) {
    auto input = design(9,7);
    auto tech = library(4); tech.vias[1].name = "tsv_bridge";
    GCellGrid grid(input,tech);
    PinDatabase pins;
    const auto a = pin(pins,grid,"a",{0,0,0}), b = pin(pins,grid,"b",{8,6,0});
    const auto c = pin(pins,grid,"c",{0,6,3}), d = pin(pins,grid,"d",{8,0,3});
    const auto e = pin(pins,grid,"e",{4,3,1}), f = pin(pins,grid,"f",{4,3,2});
    input.nets = {net("many_crossings_before_fix",{a,c,b,d}),net("shared_landing_pins",{a,e,f,d}),net("bottom_only",{a,b})};
    Resources resources(input,grid);
    auto runOptions=options(2); runOptions.astarWeight=weight;
    const auto result = route(input,pins,grid,resources,runOptions);
    for (std::size_t n=0;n<result.routes.size();++n) {
        connected(result.routes[n],grid,resources);
        std::set<int> lower,upper; std::size_t crosses=0;
        for (const auto node : resources.routeNodes(result.routes[n])) {
            const auto cell=grid.cell(node);
            if (cell.layer==1) lower.insert(cell.y*grid.width()+cell.x);
            if (cell.layer==2) upper.insert(cell.y*grid.width()+cell.x);
            if (n==2) check(cell.layer<2,"same-die net escaped into the other die");
        }
        for (const auto& edge : resources.routeEdges(result.routes[n]))
            crosses += resources.isCross(grid.cell(edge.first).layer,grid.cell(edge.second).layer);
        if (n<2) {
            std::vector<int> overlap;
            std::set_intersection(lower.begin(),lower.end(),upper.begin(),upper.end(),std::back_inserter(overlap));
            check(crosses==1 && overlap.size()==1,"all branches must share one TSV edge AND one guide overlap");
            if (n==1) check(overlap.front()==3*grid.width()+4,"shared interface pins must select their mandatory landing");
        } else check(crosses==0,"same-die net must not acquire a TSV");
    }
    bestAndUsage(result,input,grid,resources);
}

void thermalLandingBeyondLocalWindow() {
    auto input = design(19,15);
    auto tech = library(4); tech.vias[1].name = "tsv_bridge";
    GCellGrid grid(input,tech);
    PinDatabase pins;
    input.nets = {net("cold_pins", {pin(pins,grid,"bottom",{1,1,0}), pin(pins,grid,"top",{1,1,3})})};
    const auto file = std::filesystem::current_path() /
        ("contest_search_heat_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path,ignored); }
    } cleanup{file};
    {
        std::ofstream heat(file);
        heat << "{\"meta\":{\"coordinateUnit\":\"dbu\",\"ambientTemp\":25},\"nodes\":[";
        for (int y=0;y<grid.height();++y) for (int x=0;x<grid.width();++x) {
            if (x || y) heat << ',';
            const auto p=grid.center({x,y,0});
            heat << '[' << p.x << ',' << p.y << ",0," << (x==12 && y==10 ? 80 : 30) << ']';
        }
        heat << "]}";
    }
    ResourceOptions weights; weights.thermalWeight=1000; weights.presentWeight=0;
    Resources local(input,grid,weights), thermal(input,grid,weights);
    local.loadThermal(file.string()); thermal.loadThermal(file.string());
    auto localOptions=options(); localOptions.thermalSearchRadius=0;
    const auto baseline=route(input,pins,grid,local,localOptions);
    auto heatOptions=options(); heatOptions.thermalSearchRadius=16;
    const auto optimized=route(input,pins,grid,thermal,heatOptions);
    connected(baseline.routes.front(),grid,local);
    connected(optimized.routes.front(),grid,thermal);
    check(local.tsvCounts()[10*grid.width()+12]==0,"Local search unexpectedly reached the distant hotspot");
    check(thermal.tsvCounts()[10*grid.width()+12]==1,"Expanded search missed a reachable distant hotspot");
    check(thermal.correlation(true)>0.99 && thermal.correlation(true)>local.correlation(true),
          "Heat-directed landing must improve the actual above-ambient Pearson coefficient");
    std::size_t crossings=0;
    for (const auto& edge:thermal.routeEdges(optimized.routes.front()))
        crossings+=thermal.isCross(grid.cell(edge.first).layer,grid.cell(edge.second).layer);
    check(crossings==1,"Moving the landing must not add TSVs");
    const auto guide=file.string()+".guide";
    Cleanup guideCleanup{guide};
    writeGuide(guide,input,grid,thermal,optimized.routes);
    check(validateGuide(guide,input,pins,grid).passed,"Distant thermal landing must retain unique guide overlap and pin access");

    // Neither absent heat nor an explicitly disabled heat objective should
    // change the baseline candidate set or geometry.
    weights.thermalWeight=0;
    Resources disabled(input,grid,weights), noHeat(input,grid,weights);
    disabled.loadThermal(file.string());
    const auto disabledResult=route(input,pins,grid,disabled,heatOptions);
    const auto noHeatResult=route(input,pins,grid,noHeat,localOptions);
    check(sameRoutes(disabledResult.routes,noHeatResult.routes),"Disabled heat objective changed route geometry");
}

void thermalLandingReusesFanoutTree() {
    auto input=design(20,3);
    auto tech=library(4); tech.vias[1].name="tsv_bridge";
    GCellGrid grid(input,tech);
    PinDatabase pins;
    std::vector<Connection> connections;
    for (int x=1;x<=8;++x) connections.push_back(pin(pins,grid,"bottom"+std::to_string(x),{x,1,0}));
    connections.push_back(pin(pins,grid,"bottom_end",{18,1,0}));
    connections.push_back(pin(pins,grid,"top",{18,1,3}));
    input.nets={net("fanout",connections)};
    const auto file=std::filesystem::current_path()/
        ("contest_fanout_heat_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path,ignored); }
    } cleanup{file};
    {
        std::ofstream heat(file);
        heat<<"{\"meta\":{\"coordinateUnit\":\"dbu\",\"ambientTemp\":25},\"nodes\":[";
        for (int y=0;y<grid.height();++y) for (int x=0;x<grid.width();++x) {
            if (x || y) heat<<',';
            const auto p=grid.center({x,y,0});
            heat<<'['<<p.x<<','<<p.y<<",0,"<<(x==18 && y==1 ? 80 : 30)<<']';
        }
        heat<<"]}";
    }
    ResourceOptions weights; weights.thermalWeight=50; weights.presentWeight=0;
    Resources resources(input,grid,weights); resources.loadThermal(file.string());
    const auto result=route(input,pins,grid,resources,options());
    connected(result.routes.front(),grid,resources);
    check(resources.tsvCounts()[grid.width()+18]==1,"Fanout must reuse the planar tree rather than overprice distance to every pin");
    const auto guide=file.string()+".guide";
    Cleanup guideCleanup{guide};
    writeGuide(guide,input,grid,resources,result.routes);
    check(validateGuide(guide,input,pins,grid).passed,"Fanout landing must retain pin access and a unique TSV");
    Resources compared(input,grid,weights); compared.loadThermal(file.string());
    auto trialOptions=options(); trialOptions.tsvRouteTrials=4;
    const auto trials=route(input,pins,grid,compared,trialOptions);
    connected(trials.routes.front(),grid,compared);
    check(compared.staticCost(trials.routes.front())<=resources.staticCost(result.routes.front()),
          "Comparing completed TSV trees must retain the first feasible candidate when it is cheaper");
    long long committedTsvs=0;
    for (const auto count:compared.tsvCounts()) committedTsvs+=count;
    check(committedTsvs==1,"Unselected TSV candidate trees must not leave resource occupancy behind");
    writeGuide(guide,input,grid,compared,trials.routes);
    check(validateGuide(guide,input,pins,grid).passed,"Compared candidate trees must retain unique TSV guide geometry");
}

void macroBoundaryEscape() {
    auto input=design(7,5); auto tech=library(); GCellGrid grid(input,tech);
    grid.addObstacles({{"M0",{0,130,700,500}}});
    PinDatabase pins;
    pins.ports["edge"]={"PIN","","edge","SIGNAL",{{"M0",{245,110,255,120}}}};
    const auto other=pin(pins,grid,"outside",{6,0,0});
    input.nets={net("macro_edge",{{"PIN","edge"},other})};
    Resources resources(input,grid);
    auto result=route(input,pins,grid,resources,options());
    connected(result.routes.front(),grid,resources);
    const auto& terminal=result.routes.front().terminals.front();
    check(!terminal.accessPath.empty() && terminal.accessPath.front()==Cell{2,1,0},
          "Escape must retain the actual pin cell in the guide");
    check(terminal.cell==Cell{2,0,0},"Macro edge pin should escape toward the unobstructed lower cell");
    check(grid.blocked({2,1,0}),"Pin escape must not unblock a macro globally");
    check(resources.usage()[grid.id({2,1,0})]==1,"Pin escape cells must consume net resources");
    check(!grid.segmentBlocked(terminal.point,{terminal.point.x,50},0),"Escape stub crossed an actual obstacle");
    const auto file=std::filesystem::temp_directory_path()/
        ("contest_escape_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".guide");
    writeGuide(file.string(),input,grid,resources,result.routes);
    const auto validation=validateGuide(file.string(),input,pins,grid);
    std::filesystem::remove(file);
    check(validation.passed,"Escaped macro pin must be covered and connected in the written guide");
    bestAndUsage(result,input,grid,resources);
    resources.commit(result.routes.front(),-1);
    check(resources.usedCells()==0,"Rip-up must remove pin escape occupancy too");
    grid.addObstacles({{"M0",{0,90,700,105}}});
    Resources sealed(input,grid);
    const auto failed=route(input,pins,grid,sealed,options());
    check(!failed.routes.front().success,"Escape must not cross the real wall below the pin");
    auto twoDies=library(2); twoDies.vias.front().name="TSV";
    GCellGrid crossGrid(input,twoDies);
    crossGrid.addObstacles({{"M0",{0,130,700,500}},{"M0",{0,90,700,105}}});
    Resources crossResources(input,crossGrid);
    const auto crossFailed=route(input,pins,crossGrid,crossResources,options());
    check(!crossFailed.routes.front().success,"A blocked pin must not escape through the other die");
}

void pinPortalInOwnMacroObs() {
    auto input=design(7,5); auto tech=library(4); GCellGrid grid(input,tech);
    grid.addObstacles({{"M0",{0,0,700,500},"memory"},{"M1",{0,0,700,500},"memory"}});
    PinDatabase pins;
    pins.instances["memory"]["D"]={"memory","MEM","D","SIGNAL",
        {{"M0",{245,110,255,120}},{"M1",{245,110,255,120}}}};
    input.nets={net("memory_portal",{{"memory","D"},pin(pins,grid,"outside",{6,0,2})})};
    Resources resources(input,grid);
    const auto result=route(input,pins,grid,resources,options());
    connected(result.routes.front(),grid,resources);
    const auto& terminal=result.routes.front().terminals.front();
    check(terminal.cell.layer==2 && terminal.accessPath.size()==3,"Own pin metal should permit a via stack out of the macro OBS");
    check(grid.pointBlocked(terminal.point,0) && grid.blocked({2,1,0}),"Own-pin portal must not erase macro obstruction");
    bestAndUsage(result,input,grid,resources);
    // A via stack must stop if an obstructed intermediate layer has no metal
    // belonging to this pin, or if another owner also obstructs the pin.
    auto missing=pins; missing.instances["memory"]["D"].shapes.resize(1);
    Resources blocked(input,grid);
    check(!route(input,missing,grid,blocked,options()).routes.front().success,
          "Pin access must not cross an OBS on an intermediate layer without pin metal");
    grid.addObstacles({{"M1",{200,100,300,200},"neighbor"}});
    Resources neighbor(input,grid);
    check(!route(input,pins,grid,neighbor,options()).routes.front().success,
          "Pin access must not ignore an obstruction from another macro");
}
} // namespace

int main() {
    try {
        allNetTypesAndDeterminism();
        unreachableAndDeadline();
        negotiatedCongestion();
        shortestPathAgainstDijkstra();
        treeDistanceAgainstExhaustiveScan();
        uniqueTsvAndDieRestriction();
        uniqueTsvAndDieRestriction(1.5);
        thermalLandingBeyondLocalWindow();
        thermalLandingReusesFanoutTree();
        macroBoundaryEscape();
        pinPortalInOwnMacroObs();
        std::cout << "PASS: " << checks << " contest-search checks; deterministic routing, all NETS types, "
                     "deadlines, disconnected pins, negotiated congestion, best-snapshot occupancy, "
                     "64 independent A*/Dijkstra comparisons.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
