#include "router/contest_guide.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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
template <class Action>
void rejects(Action action, const std::string& message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}

struct TemporaryGuide {
    std::filesystem::path path;
    TemporaryGuide() {
        static unsigned sequence = 0;
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
               / ("contest_guide_test_" + std::to_string(stamp) + "_"
                  + std::to_string(sequence++) + ".guide");
    }
    ~TemporaryGuide() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    void write(const std::string& text) const {
        std::ofstream out(path, std::ios::trunc);
        out << text;
        if (!out) throw std::runtime_error("Cannot write guide test fixture");
    }
    std::string read() const {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Cannot read guide test fixture");
        std::ostringstream result;
        result << input.rdbuf();
        return result.str();
    }
};

Design designFixture() {
    Design design;
    design.dbuPerMicron = 100;
    design.die = {10, -20, 345, 230};
    design.gridPatterns = {{'X', 10, 100, 4}, {'X', 160, 20, 1}, {'Y', -20, 100, 3}};
    Net signal;
    signal.name = "signal[3]";
    signal.connections = {{"PIN", "A"}, {"u_1", "B"}};
    design.nets.push_back(signal);
    Net unused;
    unused.name = "unused";
    design.nets.push_back(unused);
    return design;
}

Library libraryFixture() {
    Library library;
    library.layers = {{"M0", "ROUTING", "HORIZONTAL", 0.1, 0.4},
                      {"C01", "CUT", "", 0.0, 0.0},
                      {"M1", "ROUTING", "VERTICAL", 0.1, 0.4},
                      {"M2", "ROUTING", "HORIZONTAL", 0.1, 0.4}};
    library.vias = {{"V01", true, {"M0", "C01", "M1"}}};
    return library;
}

PinDatabase pinsFixture() {
    PinDatabase pins;
    pins.ports["A"] = {"PIN", "", "A", "SIGNAL", {{"M0", {14, -16, 16, -14}}}};
    pins.instances["u_1"]["B"] = {"u_1", "CELL", "B", "SIGNAL",
                                  {{"M1", {329, -16, 331, -14}},
                                   {"M2", {211, 181, 220, 190}}}};
    return pins;
}

std::vector<RoutedNet> routesFixture() {
    RoutedNet signal;
    signal.success = true;
    signal.terminals = {{{15, -15}, {0, 0, 1}, {0, 1}},
                        {{330, -15}, {4, 0, 1}, {1}}};
    signal.branches = {{{0, 0, 1}, {1, 0, 1}, {2, 0, 1}, {3, 0, 1}, {4, 0, 1}},
                       {{2, 0, 1}, {2, 1, 1}}};
    RoutedNet unused;
    unused.success = true;
    return {signal, unused};
}

const std::string validSignal =
    "signal[3]\n(\n10 -20 110 80 M0\n10 -20 345 80 M1\n160 80 210 180 M1\n)\n";
const std::string emptyUnused = "unused\n(\n)\n";

void errorContains(const Validation& validation, const std::string& expected) {
    check(!validation.passed, "Invalid guide was accepted: " + expected);
    bool found = false;
    for (const auto& error : validation.errors)
        if (error.find(expected) != std::string::npos) found = true;
    check(found, "Missing validation diagnostic: " + expected);
}

void testWriterAndIndependentReadback() {
    const auto design = designFixture();
    const auto pins = pinsFixture();
    const auto library = libraryFixture();
    GCellGrid grid(design, library);
    grid.blockCell({0, 0, 0}); // A legal access pin shares a blocked GCELL center.
    Resources resources(design, grid);
    const auto routes = routesFixture();
    TemporaryGuide guide;
    writeGuide(guide.path.string(), design, grid, resources, routes);
    check(guide.read() == validSignal + emptyUnused,
          "Writer lost access layers, merged extra cells, or assumed uniform grid spacing");
    auto validation = validateGuide(guide.path.string(), design, pins, grid);
    check(validation.passed, "A valid route with a blocked pin-access center was rejected");
    check(validation.checkedNets == 2 && validation.rectangles == 3,
          "Validation should count all DEF nets and their guide rectangles");
    auto reorderedPins = pins;
    std::reverse(reorderedPins.instances["u_1"]["B"].shapes.begin(),
                 reorderedPins.instances["u_1"]["B"].shapes.end());
    check(validateGuide(guide.path.string(), design, reorderedPins, grid).passed,
          "Unused alternate pin shapes must not change die classification");

    // The route objects remain successful. Only the on-disk guide is damaged.
    guide.write("signal[3]\n(\n10 -20 110 80 M0\n10 -20 110 80 M1\n)\n" + emptyUnused);
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "u_1/B is not covered");

    auto failed = routes;
    failed[0].success = false;
    writeGuide(guide.path.string(), design, grid, resources, failed);
    check(guide.read() == "signal[3]\n(\n)\n" + emptyUnused,
          "Failed net must keep its name with empty guides, even with stale route data");
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "is not covered");
    rejects([&] { writeGuide(guide.path.string(), design, grid, resources, {}); },
            "A route/net count mismatch must be reported");
    auto duplicate = design;
    duplicate.nets[1].name = duplicate.nets[0].name;
    rejects([&] { writeGuide(guide.path.string(), duplicate, grid, resources, routes); },
            "Duplicate DEF net names must not overwrite one another");

    auto sparse = routes;
    sparse[0].terminals.clear();
    sparse[0].branches = {{{0, 0, 1}, {2, 0, 1}}, {{0, 1, 1}}, {{0, 0, 1}}};
    writeGuide(guide.path.string(), design, grid, resources, sparse);
    check(guide.read() == "signal[3]\n(\n10 -20 110 80 M1\n160 -20 210 80 M1\n"
                          "10 80 110 180 M1\n)\n" + emptyUnused,
          "Row merging must preserve gaps, deduplicate cells, and retain row boundaries");
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "disconnected");

    auto singleton = design;
    singleton.nets[0].connections.resize(1);
    auto pointRoute = routes;
    pointRoute[0].terminals.resize(1);
    pointRoute[0].branches.clear();
    writeGuide(guide.path.string(), singleton, grid, resources, pointRoute);
    check(guide.read() == "signal[3]\n(\n10 -20 110 80 M0\n10 -20 110 80 M1\n)\n" + emptyUnused,
          "A single-pin net still needs terminal and access-layer guide cells");
    check(validateGuide(guide.path.string(), singleton, pins, grid).passed,
          "A connected single-pin guide without branches must be valid");
}

void testMalformedAndIncompleteGuides() {
    const auto design = designFixture();
    const auto pins = pinsFixture();
    GCellGrid grid(design, libraryFixture());
    TemporaryGuide guide;
    const auto fails = [&](const std::string& text, const std::string& diagnostic) {
        guide.write(text);
        errorContains(validateGuide(guide.path.string(), design, pins, grid), diagnostic);
    };
    fails(validSignal, "Missing guide net: unused");
    fails(validSignal + emptyUnused + emptyUnused, "duplicate net");
    fails(validSignal + emptyUnused + "ghost\n(\n)\n", "unknown net");
    fails("signal[3]\n(\n10 -20 110 80 C01\n)\n" + emptyUnused, "unknown routing layer");
    fails("signal[3]\n(\n10 -20 110 80 typo\n)\n" + emptyUnused, "unknown routing layer");
    fails("signal[3]\n(\n0 -20 110 80 M0\n)\n" + emptyUnused, "outside DIEAREA");
    fails("signal[3]\n(\n10 -20 346 80 M0\n)\n" + emptyUnused, "outside DIEAREA");
    fails("signal[3]\n(\n11 -20 110 80 M0\n)\n" + emptyUnused, "not GCELLGRID aligned");
    fails("signal[3]\n(\n10 -20 10 80 M0\n)\n" + emptyUnused, "positive area");
    fails("signal[3]\n(\n110 -20 10 80 M0\n)\n" + emptyUnused, "positive area");
    fails("signal[3]\n(\n10 -20.5 110 80 M0\n)\n" + emptyUnused, "integer DBU");
    fails("signal[3]\n(\n10 -20 9999999999999999999999999 80 M0\n)\n" + emptyUnused,
          "integer DBU");
    fails("signal[3]\n(\n10 -20 110 80 M0 trailing\n)\n" + emptyUnused, "expected 'x1");
    fails("signal[3]\n(\n10 -20 110 M0\n)\n" + emptyUnused, "expected 'x1");
    fails("signal[3]\n10 -20 110 80 M0\n)\n" + emptyUnused, "expected '('");
    fails("signal[3]\n(\n10 -20 110 80 M0\n", "missing closing ')'");
    fails(validSignal + emptyUnused + ")\n", "expected a net name");

    guide.write(validSignal + emptyUnused);
    auto missingPins = pins;
    missingPins.instances.clear();
    errorContains(validateGuide(guide.path.string(), design, missingPins, grid), "Missing pin: u_1/B");
    auto noShapes = pins;
    noShapes.ports["A"].shapes.clear();
    errorContains(validateGuide(guide.path.string(), design, noShapes, grid), "PIN/A is not covered");
    auto touchingOnly = pins;
    touchingOnly.ports["A"].shapes = {{"M0", {110, -16, 112, -14}}};
    errorContains(validateGuide(guide.path.string(), design, touchingOnly, grid), "PIN/A is not covered");
    auto wrongLayer = pins;
    wrongLayer.ports["A"].shapes = {{"M2", {14, -16, 16, -14}}};
    errorContains(validateGuide(guide.path.string(), design, wrongLayer, grid), "PIN/A is not covered");
    TemporaryGuide absent;
    errorContains(validateGuide(absent.path.string(), design, pins, grid), "Cannot read guide");
}

void testConnectivityUsesActualVias() {
    auto design = designFixture();
    auto pins = pinsFixture();
    auto library = libraryFixture();
    TemporaryGuide guide;
    guide.write(validSignal + emptyUnused);
    library.vias.clear();
    GCellGrid noVias(design, library);
    errorContains(validateGuide(guide.path.string(), design, pins, noVias), "disconnected");

    // Layer numbers are not topology: a real via may connect nonconsecutive layers.
    library.vias = {{"V02", true, {"M0", "C01", "M2"}}};
    pins.instances["u_1"]["B"].shapes = {{"M2", {329, -16, 331, -14}}};
    GCellGrid nonconsecutive(design, library);
    guide.write("signal[3]\n(\n10 -20 110 80 M0\n10 -20 345 80 M2\n)\n" + emptyUnused);
    check(validateGuide(guide.path.string(), design, pins, nonconsecutive).passed,
          "An explicit LEF via between nonconsecutive layer indices must be used");

    GCellGrid grid(design, libraryFixture());
    pins = pinsFixture();
    guide.write("signal[3]\n(\n10 -20 110 80 M0\n10 -20 345 80 M1\n"
                "310 180 345 230 M1\n)\n" + emptyUnused);
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "disconnected");

    // Two diagonal cells only touch at a corner, which is not a planar grid edge.
    design.nets[0].connections.clear();
    guide.write("signal[3]\n(\n10 -20 110 80 M0\n110 80 160 180 M0\n)\n" + emptyUnused);
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "disconnected");

    // Valid syntax can include blank lines, CRLF, and signed integer coordinates.
    guide.write("\r\nsignal[3]\r\n(\r\n+10 -20 +110 +80 M0\r\n)\r\n\r\n" + emptyUnused);
    check(validateGuide(guide.path.string(), design, pins, grid).passed,
          "Whitespace or explicit positive coordinate signs were rejected");
}

void testUniqueTsvAndDieLayers() {
    auto design = designFixture();
    auto pins = pinsFixture();
    auto library = libraryFixture();
    library.layers.push_back({"M3", "ROUTING", "VERTICAL", 0.1, 0.4});
    library.vias.push_back({"TSV_12", true, {"M1", "M2"}});
    library.vias.push_back({"V23", true, {"M2", "M3"}});
    pins.ports["A"].shapes = {{"M0", {14, -16, 16, -14}}};
    pins.instances["u_1"]["B"].shapes = {{"M3", {329, -16, 331, -14}}};
    GCellGrid grid(design, library);
    TemporaryGuide guide;
    const std::string unique = "signal[3]\n(\n10 -20 110 80 M0\n10 -20 345 80 M1\n"
                               "310 -20 345 80 M2\n310 -20 345 80 M3\n)\n";
    guide.write(unique + emptyUnused);
    check(validateGuide(guide.path.string(), design, pins, grid).passed,
          "A connected two-die guide with one TSV must pass");
    // The extra overlap is connected, covers all pins, and is still illegal
    // under our unique-landing policy: connectivity alone missed this defect.
    guide.write("signal[3]\n(\n10 -20 110 80 M0\n10 -20 345 80 M1\n"
                "10 -20 110 80 M2\n310 -20 345 80 M2\n310 -20 345 80 M3\n)\n" + emptyUnused);
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "found 2");
    guide.write("signal[3]\n(\n10 -20 345 80 M0\n310 -20 345 80 M3\n)\n" + emptyUnused);
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "found 0");

    pins.instances["u_1"]["B"].shapes = {{"M1", {329, -16, 331, -14}}};
    guide.write(unique + emptyUnused);
    errorContains(validateGuide(guide.path.string(), design, pins, grid), "same-die net uses layers");

    library.vias[1].name = "package_bridge";
    GCellGrid namedBridge(design, library);
    errorContains(validateGuide(guide.path.string(), design, pins, namedBridge, {"package_bridge"}),
                  "same-die net uses layers");
}

} // namespace

int main() {
    try {
        testWriterAndIndependentReadback();
        testMalformedAndIncompleteGuides();
        testConnectivityUsesActualVias();
        testUniqueTsvAndDieLayers();
        std::cout << "PASS: " << checks << " guide writer/readback checks.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL after " << checks << " guide checks: " << exception.what() << '\n';
        return 1;
    }
}
