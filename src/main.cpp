#include "router/def_parser.hpp"
#include "router/def_writer.hpp"
#include "router/lef_parser.hpp"
#include "router/net_router.hpp"
#include "router/pin_resolver.hpp"
#include "router/tokenizer.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
struct Options {
    std::string def, lef, out, pins;
    router::GridOptions grid;
};
void help() {
    std::cout << "Learning GCELL A* router (C++17)\n"
              << "Usage: learning_router --lef cells.lef --def placed.def --out routed.def\n"
              << "       [--pins pins.csv] [--gcell-step DBU] [--via-cost 5]\n"
              << "       [--capacity 8] [--congestion-weight 2]\n"
              << "Output is an educational global-routing sketch, not DRC-clean detailed routing.\n"
              << "Exit: 0 = all eligible nets routed, 2 = some nets unrouted, 1 = input/output error.\n";
}
Options arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 == argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--def") options.def = value;
        else if (key == "--lef") options.lef = value;
        else if (key == "--out") options.out = value;
        else if (key == "--pins") options.pins = value;
        else if (key == "--gcell-step") {
            options.grid.fallbackStep = router::integer(value);
            if (options.grid.fallbackStep <= 0) throw std::runtime_error("--gcell-step must be positive");
        } else if (key == "--via-cost") options.grid.viaCost = router::real(value);
        else if (key == "--congestion-weight") options.grid.congestionWeight = router::real(value);
        else if (key == "--capacity") {
            const auto n = router::integer(value);
            if (n <= 0 || n > 1000000) throw std::runtime_error("Invalid capacity");
            options.grid.capacity = static_cast<int>(n);
        } else throw std::runtime_error("Unknown argument: " + key);
    }
    if (options.def.empty() || options.lef.empty() || options.out.empty())
        throw std::runtime_error("--lef, --def and --out are required");
    if (options.pins.empty()) {
        auto path = std::filesystem::path(options.out);
        path.replace_extension(".pins.csv");
        options.pins = path.string();
    }
    // 防止把用户的原始输入当输出覆盖；其它已有输出可重复更新。
    const auto same = [](const std::string& a, const std::string& b) {
        if (std::filesystem::exists(a) && std::filesystem::exists(b))
            return std::filesystem::equivalent(a, b);
        return std::filesystem::weakly_canonical(a) == std::filesystem::weakly_canonical(b);
    };
    if (same(options.out, options.def) || same(options.out, options.lef)
        || same(options.pins, options.def) || same(options.pins, options.lef)
        || same(options.out, options.pins))
        throw std::runtime_error("Input DEF/LEF, output DEF and pin CSV must use different paths");
    return options;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) { help(); return 0; }
    try {
        const auto options = arguments(argc, argv);
        const auto library = router::parseLef(options.lef);
        const auto design = router::parseDef(options.def);
        const auto pins = router::resolvePins(design, library);
        router::GCellGrid grid(design, library, options.grid);
        grid.addObstacles(pins.obstructions);
        grid.addObstacles(design.blockages);
        std::size_t instancePins = 0, rectangles = 0;
        for (const auto& inst : pins.instances)
            for (const auto& pin : inst.second) { ++instancePins; rectangles += pin.second.shapes.size(); }
        std::cout << "Design: " << design.name << ", DBU/micron: " << design.dbuPerMicron << '\n'
                  << "Instances: " << design.instances.size() << ", instance pins: " << instancePins
                  << ", pin rectangles: " << rectangles << ", IO pins: " << pins.ports.size() << '\n'
                  << "GCELL: " << grid.width() << " x " << grid.height() << " x " << grid.layerCount()
                  << " routing layers\n";
        const auto routes = router::routeNets(design, pins, grid);
        std::size_t routed = 0, skipped = 0, failed = 0, expanded = 0;
        for (const auto& route : routes) {
            if (route.success) ++routed;
            else if (route.skipped) ++skipped;
            else ++failed;
            expanded += route.expanded;
        }
        router::writeRoutedDef(options.out, design, grid, routes);
        router::writePinsCsv(options.pins, pins);
        std::cout << "Nets: routed=" << routed << ", skipped=" << skipped << ", failed=" << failed
                  << ", expanded=" << expanded << ", overflow edges=" << grid.overflowEdges() << '\n'
                  << "DEF: " << options.out << "\nPins: " << options.pins << '\n';
        return failed == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
