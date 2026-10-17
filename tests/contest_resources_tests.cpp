#include "router/contest_resources.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace router;
using namespace router::contest;

namespace {
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const std::string& message) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= 1.0e-9, message);
}
template<class Action> void rejects(Action action, const std::string& message) {
    bool caught = false;
    try { action(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}

const std::string uniformTracks =
    "TRACKS Y 500 DO 2 STEP 1000 LAYER M1 ;\n"
    "TRACKS X 500 DO 3 STEP 1000 LAYER M2 ;\n";

struct Fixture {
    Design design;
    Library library;
    std::unique_ptr<GCellGrid> grid;
    Fixture(const std::string& tracks = uniformTracks, double pitch = 0.2,
            const std::string& viaName = "hb_bridge") {
        design.name = "resources";
        design.dbuPerMicron = 1000;
        design.die = {0, 0, 3000, 2000};
        design.gridPatterns = {{'X', 0, 1000, 4}, {'Y', 0, 1000, 3}};
        design.source = "VERSION 5.8 ;\nDESIGN resources ;\n" + tracks + "END DESIGN\n";
        library.layers = {{"M1", "ROUTING", "HORIZONTAL", 0.05, pitch},
                          {"C", "CUT", "", 0, 0},
                          {"M2", "ROUTING", "VERTICAL", 0.05, pitch}};
        library.vias = {{viaName, true, {"M1", "C", "M2"}}};
        grid = std::make_unique<GCellGrid>(design, library);
    }
};

struct Files {
    std::vector<std::filesystem::path> paths;
    std::string prefix = "contest_resources_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string write(const std::string& contents) {
        auto path = std::filesystem::current_path() / (prefix + "_" + std::to_string(paths.size()) + ".json");
        paths.push_back(path);
        std::ofstream output(path, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot create thermal test fixture");
        output << contents;
        output.close();
        if (!output) throw std::runtime_error("Cannot write thermal test fixture");
        return path.string();
    }
    ~Files() {
        for (const auto& path : paths) { std::error_code ignored; std::filesystem::remove(path, ignored); }
    }
};

std::string thermalJson(const std::vector<double>& temperature, const std::string& units = "micron",
                        bool twoDies = false, double ambient = 25) {
    std::ostringstream out;
    out << "{\"meta\":{\"coordinateUnit\":\"" << units << "\",\"temperatureUnit\":\"degC\","
        << "\"synthetic\":true,\"ambientTemp\":" << ambient << "},\"nodes\":[";
    bool first = true;
    for (int die = 0; die < (twoDies ? 2 : 1); ++die) for (std::size_t i = 0; i < temperature.size(); ++i) {
        if (!first) out << ',';
        first = false;
        const double scale = units == "dbu" ? 1000 : 1;
        out << '[' << (static_cast<double>(i % 3) + 0.5) * scale << ','
            << (static_cast<double>(i / 3) + 0.5) * scale << ',' << die << ',' << temperature[i] + die * 2 << ']';
    }
    out << "]}";
    return out.str();
}

RoutedNet crossRoute(int x = 0, int y = 0) {
    RoutedNet route;
    route.success = true;
    route.branches = {{{x, y, 0}, {x, y, 1}}};
    return route;
}

void capacities() {
    Fixture fixture(
        "TRACKS Y 0 DO 5 STEP 500 LAYER M1 ;\n"
        "TRACKS Y 500 DO 4 STEP 500 LAYER M1 ;\n"
        "TRACKS X 0 DO 31 STEP 100 LAYER M1 ;\n"
        "TRACKS X 0 DO 7 STEP 500 LAYER M2 ;\n"
        "TRACKS X 1000 DO 5 STEP 500 LAYER M2 ;\n");
    Resources resources(fixture.design, *fixture.grid);
    for (int x = 0; x < 3; ++x) {
        check(resources.capacities()[fixture.grid->id({x, 0, 0})] == 2, "overlapping horizontal TRACKS must be deduplicated");
        check(resources.capacities()[fixture.grid->id({x, 1, 0})] == 3, "outer DIEAREA boundary track belongs to last row");
    }
    for (int y = 0; y < 2; ++y) {
        check(resources.capacities()[fixture.grid->id({0, y, 1})] == 2, "vertical TRACKS use X bins");
        check(resources.capacities()[fixture.grid->id({1, y, 1})] == 2, "internal boundary belongs to upper bin only");
        check(resources.capacities()[fixture.grid->id({2, y, 1})] == 3, "outer X track must be counted once");
    }
    ResourceOptions scaled;
    scaled.capacityScale = 0.01;
    Resources stress(fixture.design, *fixture.grid, scaled);
    for (const auto capacity : stress.capacities()) check(capacity == 1, "stress scaling preserves positive reachability");

    Fixture sparse("TRACKS Y 0 DO 1 STEP 500 LAYER M1 ;\nTRACKS X 500 DO 3 STEP 1000 LAYER M2 ;\n");
    Resources holes(sparse.design, *sparse.grid, scaled);
    check(holes.capacities()[sparse.grid->id({0, 1, 0})] == 0, "declared but absent tracks must remain zero capacity");
    check(!holes.allowed({0, 0, 0}, {0, 1, 0}), "zero-capacity resource cannot be traversed");
    check(holes.allowed({0, 0, 0}, {1, 0, 0}), "neighbor with positive capacities remains allowed");
    check(!holes.allowed({0, 0, 0}, {2, 0, 0}), "non-adjacent planar jump is not allowed");
    check(!holes.allowed({0, 0, 0}, {1, 0, 1}), "diagonal layer jump is not allowed");

    Fixture missing("");
    Resources estimated(missing.design, *missing.grid);
    for (const auto capacity : estimated.capacities()) check(capacity == 5, "decimal LEF pitch must estimate exact capacity without rounding down");
    check(estimated.warnings().size() >= 2, "missing TRACKS capacity estimates must be disclosed");
    Fixture noPitch("", 0);
    ResourceOptions fallback;
    fallback.fallbackCapacity = 10;
    fallback.capacityScale = 0.3;
    Resources defaults(noPitch.design, *noPitch.grid, fallback);
    for (const auto capacity : defaults.capacities()) check(capacity == 3, "decimal capacity scale must not lose a track to floating-point roundoff");
    Fixture bad("TRACKS X 0 DO 3 STEP 500 LAYER UNKNOWN ;\n");
    rejects([&] { Resources value(bad.design, *bad.grid); }, "unknown TRACKS layer must fail");
    Fixture negativeStep("TRACKS X 0 DO 3 STEP -500 LAYER M2 ;\n");
    rejects([&] { Resources value(negativeStep.design, *negativeStep.grid); }, "negative TRACKS step must fail");
}

void crossRecognition() {
    Fixture fixture;
    Resources resources(fixture.design, *fixture.grid);
    check(resources.isCross(0, 1) && resources.isCross(1, 0), "inferred HB crossing must be symmetric");
    check(resources.crossViaNames().count("hb_bridge") == 1, "inferred cross name must be reported");
    ResourceOptions explicitName;
    explicitName.crossVias.insert("unselected_tsv");
    rejects([&] { Resources value(fixture.design, *fixture.grid, explicitName); }, "unmatched explicit crossing must fail");
    Fixture ordinary(uniformTracks, 0.2, "via1");
    Resources plain(ordinary.design, *ordinary.grid);
    check(!plain.isCross(0, 1), "ordinary VIA must not be counted as TSV");
    explicitName.crossVias = {"via1"};
    Resources explicitCross(ordinary.design, *ordinary.grid, explicitName);
    check(explicitCross.isCross(0, 1), "explicit selected VIA can define die crossing");
    ResourceOptions invalid;
    invalid.crossViaCost = 4;
    rejects([&] { Resources value(fixture.design, *fixture.grid, invalid); }, "cross-die cost cannot violate A* lower bound");
    invalid = ResourceOptions{};
    invalid.presentWeight = -1;
    rejects([&] { Resources value(fixture.design, *fixture.grid, invalid); }, "negative congestion weight must fail");
}

void accounting() {
    Fixture fixture;
    Resources resources(fixture.design, *fixture.grid);
    auto route = crossRoute();
    route.branches.push_back({{0, 0, 1}, {0, 0, 0}});
    route.terminals.push_back({{500, 500}, {0, 0, 1}, {0, 1}});
    check(resources.routeEdges(route).size() == 1, "branch, reversed edge and pin access must share one physical VIA");
    check(resources.routeNodes(route).size() == 2, "repeated terminal cells must be counted once per net");
    fixture.grid->blockCell({0, 0, 0});
    check(resources.allowed({0, 0, 0}, {0, 0, 1}), "resource checks must preserve real-pin access through a blocked GCell center");
    resources.commit(route);
    check(resources.tsvCounts()[0] == 1, "duplicate crossing must contribute one TSV");
    check(resources.usage()[fixture.grid->id({0, 0, 0})] == 1, "access source layer must be occupied");
    check(resources.usage()[fixture.grid->id({0, 0, 1})] == 1, "access destination must be occupied once");
    check(resources.usedCells() == 2 && resources.overflow() == 0, "single net should fit unit resources");
    const auto staticBefore = resources.staticCost(route);
    resources.commit(route);
    near(resources.staticCost(route), staticBefore, "static objective must exclude dynamic congestion");
    check(resources.overflow() == 2 && resources.maxOverflow() == 1 && resources.overflowCells() == 2,
          "overflow metrics must count excess per GCell-layer");
    const auto beforeHistory = resources.stepCost({0, 0, 0}, {0, 0, 1});
    resources.updateHistory();
    check(resources.stepCost({0, 0, 0}, {0, 0, 1}) > beforeHistory, "overflow must increase history penalty");
    resources.commit(route, -1);
    resources.commit(route, -1);
    check(resources.usedCells() == 0 && resources.tsvCounts()[0] == 0, "rip-up must restore all cell and TSV counters");
    const auto usage = resources.usage();
    const auto tsv = resources.tsvCounts();
    rejects([&] { resources.commit(route, -1); }, "uncommitted route removal must fail");
    check(resources.usage() == usage && resources.tsvCounts() == tsv, "failed removal must be atomic");
    rejects([&] { resources.commit(route, 2); }, "commit only accepts signed unit increments");
    route.success = false;
    resources.commit(route);
    check(resources.usedCells() == 0, "failed routes must not reserve resources");
    route.success = true; route.skipped = true;
    resources.commit(route);
    check(resources.usedCells() == 0, "skipped routes must not reserve resources");
    RoutedNet empty;
    empty.success = true;
    resources.commit(empty); resources.commit(empty, -1);
    check(resources.usedCells() == 0, "successful zero-connection route must be a no-op");

    Resources atomic(fixture.design, *fixture.grid);
    RoutedNet lower, upper;
    lower.success = upper.success = true;
    lower.branches = {{{0, 0, 0}, {1, 0, 0}}};
    upper.branches = {{{0, 0, 1}, {0, 1, 1}}};
    atomic.commit(lower); atomic.commit(upper);
    const auto occupied = atomic.usage();
    auto fakeCross = crossRoute();
    rejects([&] { atomic.commit(fakeCross, -1); }, "TSV underflow must fail even when all route cells are occupied");
    check(atomic.usage() == occupied && atomic.tsvCounts()[0] == 0, "TSV check must precede all usage mutations");
    RoutedNet partiallyOccupied;
    partiallyOccupied.success = true;
    partiallyOccupied.branches = {{{0, 0, 0}, {0, 1, 0}}};
    rejects([&] { atomic.commit(partiallyOccupied, -1); }, "partially occupied removal must fail");
    check(atomic.usage() == occupied, "partial cell-underflow failure must not decrement earlier cells");
    atomic.clearUsage();
    check(atomic.usedCells() == 0 && atomic.overflow() == 0, "clearUsage resets current demand");
    rejects([&] { atomic.setPresentFactor(-0.1); }, "negative present factor must fail");
}

void thermal() {
    Fixture fixture;
    Files files;
    ResourceOptions weights;
    weights.presentWeight = 0;
    Resources resources(fixture.design, *fixture.grid, weights);
    const auto flat = files.write(thermalJson({30, 30, 30, 30, 30, 30}, "micron", true));
    resources.loadThermal(flat);
    check(resources.thermal().loaded && resources.thermal().synthetic, "synthetic thermal metadata must be retained");
    check(resources.thermal().mappedNodes == 12, "all per-die thermal samples must be counted");
    for (const auto value : resources.thermal().temperature) near(value, 31, "die temperatures must be equally averaged");
    resources.commit(crossRoute());
    near(resources.correlation(), 0, "uniform temperature Pearson coefficient must be zero");
    near(resources.correlation(true), 0, "uniform above-ambient Pearson coefficient must be zero");
    resources.clearUsage();
    resources.loadThermal(files.write(thermalJson({30, 30, 30, 30, 30, 60})));
    const auto cold = resources.stepCost({0, 0, 0}, {0, 0, 1});
    const auto hot = resources.stepCost({2, 1, 0}, {2, 1, 1});
    check(hot < cold, "thermal term must prefer hotter TSV locations");
    check(hot >= fixture.grid->options().viaCost, "hotspot preference must preserve positive A* lower bounds");
    near(resources.correlation(), 0, "zero TSV variance must produce zero correlation");
    resources.commit(crossRoute(2, 1));
    near(resources.correlation(), 1, "one-hot temperature and density vectors should correlate perfectly");
    near(resources.correlation(true), 1, "above-ambient correlation should match this positive-temperature example");
    Resources inDbu(fixture.design, *fixture.grid, weights);
    inDbu.loadThermal(files.write(thermalJson({30, 30, 30, 30, 30, 60}, "dbu")));
    check(inDbu.thermal().temperature == resources.thermal().temperature, "DBU and micron maps must agree");
    const auto previous = resources.thermal().temperature;
    const auto warningCount = resources.warnings().size();
    rejects([&] { resources.loadThermal(files.write("{\"nodes\":[]}")); }, "empty sampling must fail");
    check(resources.thermal().temperature == previous && resources.warnings().size() == warningCount,
          "failed thermal replacement must preserve the existing map and warnings");
    rejects([&] { resources.loadThermal(files.write("{\"nodes\":[[9,0.5,0,30]]}")); }, "out-of-die coordinate must fail");
    rejects([&] { resources.loadThermal(files.write("{\"nodes\":[[0.5,0.5,0,NaN]]}")); }, "NaN must be rejected as invalid JSON");
    rejects([&] { resources.loadThermal(files.write("{\"nodes\":[[0.5,0.5,0,1e999]]}")); }, "overflowed temperature must fail");
    rejects([&] { resources.loadThermal(files.write("{\"nodes\":[[0.5,0.5,0,\"30\"]]}")); }, "numeric strings must not be silently accepted");
    resources.loadThermal(files.write("{\"nodes\":[[0.5,0.5,0,30],[0.6,0.6,0,32]]}"));
    for (const auto value : resources.thermal().temperature) near(value,31,"same-cell samples must be averaged before filling");
    rejects([&] { resources.loadThermal(files.write(thermalJson({30, 30, 30, 30, 30, 30}, "meter"))); }, "unsupported coordinate units must fail");
    rejects([&] { resources.loadThermal(files.write("{\"meta\":{\"ambientTemp\":25,\"ambientTemp\":30},\"nodes\":[]}")); }, "duplicate metadata keys must fail");
    rejects([&] { resources.loadThermal(files.write(thermalJson({30, 30, 30, 30, 30, 30}) + " trailing")); }, "trailing non-JSON text must fail");
    resources.loadThermal(files.write(thermalJson({10, 10, 10, 10, 10, 20}, "micron", false, 25)));
    near(resources.correlation(true), 0, "all temperatures below ambient must have zero excess-temperature variance");
}

void sparseThermalAndLargeGrid() {
    Fixture fixture;
    Files files;
    Resources resources(fixture.design,*fixture.grid);
    resources.loadThermal(files.write("{\"meta\":{\"ambientTemp\":25},\"nodes\":[[0.5,0.5,0,10],[2.5,0.5,0,30],[0.5,1.5,0,40]],\"cells\":[{\"ignored\":[1,2,3]}]}"));
    check(resources.thermal().temperature == std::vector<double>({10,10,30,40,40,40}),
          "row-first sparse fill must preserve each sampled row before propagating vertically");
    check(resources.thermal().sampledCells == 3 && resources.thermal().filledCells == 3,"thermal coverage diagnostics wrong");
    resources.loadThermal(files.write("{\"nodes\":[[1.5,1.5,0,42]]}"));
    for (const auto value : resources.thermal().temperature) near(value,42,"leading holes must use the first available sample");
    resources.loadThermal(files.write("{\"nodes\":[[3.06,2,0,35]]}"));
    check(resources.thermal().clippedNodes == 1,"60 DBU boundary excess must be recorded");
    ResourceOptions strict; strict.thermalBoundaryToleranceDbu = 0;
    Resources exact(fixture.design,*fixture.grid,strict);
    rejects([&]{exact.loadThermal(files.write("{\"nodes\":[[3.06,2,0,35]]}"));},"explicit zero edge tolerance must be honored");
    auto shifted = fixture.design;
    shifted.die = {-100,-200,2900,1800};
    shifted.gridPatterns = {{'X',-100,1000,4},{'Y',-200,1000,3}};
    GCellGrid localGrid(shifted,fixture.library);
    Resources local(shifted,localGrid);
    local.loadThermal(files.write("{\"nodes\":[[0,0,0,10],[3,0,0,20],[0,2,0,30],[3,2,0,40]]}"));
    check(local.thermal().coordinates == "die-local" && local.thermal().offsetX == -100 && local.thermal().offsetY == -200,
          "local thermal origin must be translated for nonzero DIEAREA");
    check(local.thermal().temperature == std::vector<double>({10,10,20,30,30,40}),"translated sparse thermal map differs");
    auto decimal = shifted;
    decimal.die = {-2172,-2172,1739718,1685420};
    decimal.gridPatterns = {{'X',-2172,600000,3},{'Y',-2172,600000,3}};
    GCellGrid decimalGrid(decimal,fixture.library);
    Resources decimalBoundary(decimal,decimalGrid,strict);
    decimalBoundary.loadThermal(files.write("{\"nodes\":[[0,0,0,10],[1741.89,1687.592,0,20]]}"));
    check(decimalBoundary.thermal().coordinates == "die-local" && decimalBoundary.thermal().clippedNodes == 0,
          "double rounding at an exact decimal boundary is not physical clipping, even at zero tolerance");
    GridOptions budget; budget.maxNodes = 50000000;
    auto large = fixture.design;
    large.die = {0,0,1379000,1346000};
    large.gridPatterns = {{'X',0,1000,1380},{'Y',0,1000,1347}};
    Library tech;
    for (int i=0;i<20;++i) tech.layers.push_back({"L"+std::to_string(i),"ROUTING","HORIZONTAL",.1,1});
    GCellGrid largeGrid(large,tech,budget);
    check(largeGrid.size()==37122680,"case005-sized grid must fit configured node and array budgets");
    check(largeGrid.id(largeGrid.cell(largeGrid.size()-1))==largeGrid.size()-1,"large-grid ID round trip failed");
    budget.denseMemoryMiB=100;
    rejects([&]{GCellGrid denied(large,tech,budget);},"dense-array budget must still be enforced");
}
} // namespace

int main() {
    try {
        capacities(); crossRecognition(); accounting(); thermal(); sparseThermalAndLargeGrid();
        std::cout << "contest resources: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "contest resources test failed after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
