#include "router/contest_guide.hpp"
#include "router/contest_router.hpp"
#include "router/def_parser.hpp"
#include "router/lef_parser.hpp"
#include "router/pin_resolver.hpp"
#include "router/tokenizer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
namespace rc = router::contest;
namespace {
struct Arguments {
    std::vector<std::string> lefs;
    std::string def, thermal, out, report, check;
    router::GridOptions grid;
    rc::ResourceOptions resources;
    rc::Options routing;
    Arguments() { grid.viaCost = 1.0; grid.congestionWeight = 0; grid.maxNodes = 50000000; }
};
std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    return out.str() + '"';
}
template<class Container> std::string strings(const Container& values) {
    std::string text = "[";
    for (const auto& value : values) { if (text.size() > 1) text += ", "; text += quote(value); }
    return text + "]";
}
void help() {
    std::cout <<
        "3DIC multi-objective global router 1.4 (C++17)\n"
        "Usage: contest_router --lef tech.lef [--lef cells.lef] --def placed.def\n"
        "       --out result.guide [--thermal heat.json] [--report result.json]\n"
        "Check: contest_router --lef tech.lef --def placed.def --check result.guide\n"
        "Options:\n"
        "  --iterations N           reroute rounds after initial routing (8)\n"
        "  --time-limit SECONDS     soft budget incl. input setup; output extra (300)\n"
        "  --order short|def        initial net ordering (short); nearest-tree pins\n"
        "  --astar-weight N         search weight in [1,3] (1); >1 trades path optimality for speed\n"
        "  --via-cost N             ordinary via equivalent cost in microns (1)\n"
        "  --cross-via-cost N       cross-die cost >= via-cost (8)\n"
        "  --cross-via NAME         repeatable explicit cross-die LEF VIA name\n"
        "                          default: selected VIA names containing hb/tsv/hybrid\n"
        "  --thermal-weight N       nonnegative penalty for cold cross-die sites (600)\n"
        "  --thermal-search-radius N extra TSV search radius in GCells (64; 0 disables)\n"
        "  --tsv-route-trials N     successful TSV candidate trees to compare, 1..32 (1)\n"
        "  --present-weight N       present congestion weight (2)\n"
        "  --history-weight N       accumulated overflow weight (1)\n"
        "  --capacity-scale N       track capacity multiplier in (0,1] (1)\n"
        "  --fallback-capacity N    capacity if no TRACKS or LEF pitch (8)\n"
        "  --gcell-step DBU         explicit fallback if GCELLGRID is missing\n"
        "  --max-grid-nodes N       grid node budget (50000000)\n"
        "  --grid-memory-mb N       dense-array memory budget in MiB (4096)\n"
        "  --thermal-coordinates MODE auto|absolute|die-local (auto)\n"
        "  --thermal-boundary-tolerance-dbu N  edge clipping tolerance (100)\n"
        "Output: ISPD-style guides + JSON local metrics; no official score or DRC claim.\n"
        "Exit: 0 local guide checks pass; 2 incomplete/invalid; 1 input/output error.\n";
}
bool samePath(const std::string& a, const std::string& b) {
    if (fs::exists(a) && fs::exists(b)) return fs::equivalent(a, b);
#ifdef _WIN32
    const auto first = fs::weakly_canonical(a).wstring();
    const auto second = fs::weakly_canonical(b).wstring();
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return fs::weakly_canonical(a) == fs::weakly_canonical(b);
#endif
}
void checkOutputPath(const std::string& output) {
#ifdef _WIN32
    const fs::path path(output);
    for (const auto& component : path.relative_path()) {
        auto name = component.wstring();
        if (name == L"." || name == L".." || name.empty()) continue;
        if (name.back() == L'.' || name.back() == L' ' || name.find(L':') != std::wstring::npos)
            throw std::runtime_error("Ambiguous Windows output path component: " + component.string());
        name = name.substr(0, name.find(L'.'));
        for (auto& c : name) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
        if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL"
            || (name.size() == 4 && (name.substr(0, 3) == L"COM" || name.substr(0, 3) == L"LPT")
                && name[3] >= L'1' && name[3] <= L'9'))
            throw std::runtime_error("Reserved Windows output path: " + component.string());
    }
#else
    (void)output;
#endif
}
Arguments arguments(int argc, char** argv) {
    Arguments a;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 == argc) throw std::runtime_error("Missing value for " + key);
        const std::string v = argv[++i];
        if (key == "--lef") a.lefs.push_back(v);
        else if (key == "--def") a.def = v;
        else if (key == "--thermal") a.thermal = v;
        else if (key == "--out") a.out = v;
        else if (key == "--report") a.report = v;
        else if (key == "--check") a.check = v;
        else if (key == "--cross-via") a.resources.crossVias.insert(v);
        else if (key == "--thermal-coordinates") {
            if (v != "auto" && v != "absolute" && v != "die-local")
                throw std::runtime_error("--thermal-coordinates must be auto, absolute or die-local");
            a.resources.thermalCoordinates = v;
        } else if (key == "--max-grid-nodes" || key == "--grid-memory-mb") {
            const auto n = router::integer(v);
            if (n <= 0) throw std::runtime_error(key + " must be positive");
            if (key == "--max-grid-nodes") a.grid.maxNodes = n;
            else a.grid.denseMemoryMiB = n;
        }
        else if (key == "--order") {
            if (v != "short" && v != "def") throw std::runtime_error("--order must be short or def");
            a.routing.inputOrder = v == "def";
        } else if (key == "--iterations" || key == "--fallback-capacity" || key == "--thermal-search-radius" || key == "--tsv-route-trials") {
            const auto n = router::integer(v);
            if (key == "--iterations") {
                if (n < 0 || n > 1000) throw std::runtime_error("--iterations must be 0..1000");
                a.routing.iterations = static_cast<int>(n);
            } else if (key == "--thermal-search-radius") {
                if (n < 0 || n > 4096) throw std::runtime_error("--thermal-search-radius must be 0..4096");
                a.routing.thermalSearchRadius = static_cast<int>(n);
            } else if (key == "--tsv-route-trials") {
                if (n<1 || n>32) throw std::runtime_error("--tsv-route-trials must be 1..32");
                a.routing.tsvRouteTrials=static_cast<int>(n);
            } else {
                if (n < 1 || n > 1000000) throw std::runtime_error("Invalid fallback capacity");
                a.resources.fallbackCapacity = static_cast<int>(n);
            }
        } else if (key == "--gcell-step") {
            a.grid.fallbackStep = router::integer(v);
            if (a.grid.fallbackStep <= 0) throw std::runtime_error("--gcell-step must be positive");
        } else {
            const double n = router::real(v);
            if (!std::isfinite(n) || n < 0 || n > 1.0e9) throw std::runtime_error(key + " requires a finite value in [0, 1e9]");
            if (key == "--time-limit") a.routing.timeLimitSeconds = n;
            else if (key == "--astar-weight") a.routing.astarWeight = n;
            else if (key == "--via-cost") a.grid.viaCost = n;
            else if (key == "--cross-via-cost") a.resources.crossViaCost = n;
            else if (key == "--thermal-weight") a.resources.thermalWeight = n;
            else if (key == "--present-weight") a.resources.presentWeight = n;
            else if (key == "--history-weight") a.resources.historyWeight = n;
            else if (key == "--capacity-scale") a.resources.capacityScale = n;
            else if (key == "--thermal-boundary-tolerance-dbu") a.resources.thermalBoundaryToleranceDbu = n;
            else throw std::runtime_error("Unknown argument: " + key);
        }
    }
    if (a.lefs.empty() || a.def.empty()) throw std::runtime_error("--lef and --def are required");
    if (a.check.empty() == a.out.empty()) throw std::runtime_error("Specify exactly one of --out or --check");
    if (a.grid.viaCost <= 0 || a.resources.crossViaCost < a.grid.viaCost)
        throw std::runtime_error("Require 0 < via-cost <= cross-via-cost");
    if (a.resources.capacityScale <= 0 || a.resources.capacityScale > 1)
        throw std::runtime_error("--capacity-scale must be in (0,1]");
    if (a.routing.astarWeight < 1 || a.routing.astarWeight > 3)
        throw std::runtime_error("--astar-weight must be in [1,3]");
    if (a.report.empty()) a.report = (a.out.empty() ? a.check + ".validation" : a.out) + ".json";
    auto inputs = a.lefs;
    inputs.push_back(a.def);
    if (!a.thermal.empty()) inputs.push_back(a.thermal);
    if (!a.check.empty()) inputs.push_back(a.check);
    std::vector<std::string> outputs{a.report};
    if (!a.out.empty()) outputs.push_back(a.out);
    for (const auto& output : outputs) {
        checkOutputPath(output);
        for (const auto& input : inputs)
            if (samePath(output, input)) throw std::runtime_error("Output must not overwrite any input: " + output);
        if (fs::is_directory(output)) throw std::runtime_error("Output is a directory: " + output);
    }
    if (!a.out.empty() && samePath(a.out, a.report)) throw std::runtime_error("Guide and report paths must differ");
    return a;
}
router::Library loadLibrary(const std::vector<std::string>& paths) {
    router::Library merged;
    for (const auto& path : paths) {
        auto part = router::parseLef(path);
        merged.normalizedRectangles += part.normalizedRectangles;
        for (auto& layer : part.layers) {
            const auto it = std::find_if(merged.layers.begin(), merged.layers.end(), [&](const auto& x) { return x.name == layer.name; });
            if (it == merged.layers.end()) merged.layers.push_back(std::move(layer));
            else if (it->type != layer.type || it->direction != layer.direction || it->width != layer.width || it->pitch != layer.pitch)
                throw std::runtime_error("Conflicting LEF layer: " + layer.name);
        }
        for (auto& item : part.macros)
            if (!merged.macros.emplace(item.first, std::move(item.second)).second)
                throw std::runtime_error("Duplicate LEF macro: " + item.first);
        for (auto& via : part.vias) {
            const auto it = std::find_if(merged.vias.begin(), merged.vias.end(), [&](const auto& x) { return x.name == via.name; });
            if (it == merged.vias.end()) merged.vias.push_back(std::move(via));
            else if (it->layers != via.layers || it->isDefault != via.isDefault)
                throw std::runtime_error("Conflicting LEF via: " + via.name);
        }
    }
    return merged;
}
double peakMemoryMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        return static_cast<double>(memory.PeakWorkingSetSize) / (1024 * 1024);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
        return static_cast<double>(usage.ru_maxrss) / (1024 * 1024);
#else
        return static_cast<double>(usage.ru_maxrss) / 1024;
#endif
    }
#endif
    return 0;
}
// Unique sibling temporaries avoid truncating an earlier complete result on failed input.
class PendingFile {
public:
    std::string temporary, target;
    explicit PendingFile(std::string path) : target(std::move(path)) {
        if (!fs::path(target).parent_path().empty()) fs::create_directories(fs::path(target).parent_path());
        temporary = target + ".tmp." + std::to_string(Clock::now().time_since_epoch().count());
        if (fs::exists(temporary)) throw std::runtime_error("Temporary file collision");
    }
    ~PendingFile() { std::error_code error; fs::remove(temporary, error); }
    void publish() {
#ifdef _WIN32
        if (!MoveFileExW(fs::path(temporary).c_str(), fs::path(target).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace output: " + target);
#else
        fs::rename(temporary, target);
#endif
    }
};
void writeText(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out || !(out << content) || !out.flush()) throw std::runtime_error("Cannot write " + path);
}
std::string validationJson(const rc::Validation& v) {
    std::ostringstream out;
    out << "{\"passed\": " << (v.passed ? "true" : "false")
        << ", \"checked_nets\": " << v.checkedNets << ", \"rectangles\": " << v.rectangles
        << ", \"scope\": \"guide format, bounds, connectivity, pins, die layers and unique TSV (stricter than scorer two-candidate exceptions)\""
        << ", \"drc_verified\": false, \"errors\": " << strings(v.errors) << '}';
    return out.str();
}
std::string reportJson(const Arguments& a, const router::Design& design,
                       const router::GCellGrid& grid, const rc::Resources& resources,
                       const rc::Result& result, const rc::Validation& validation,
                       double seconds, const router::Library& library) {
    std::size_t routed = 0, failed = 0, vias = 0, cross = 0;
    double planar = 0, hpwl = 0, objective = 0;
    std::vector<std::string> failures, warnings = resources.warnings();
    std::size_t escapedTerminals=0;
    for (const auto& r : result.routes) for (const auto& terminal : r.terminals)
        if (!terminal.accessPath.empty()) ++escapedTerminals;
    if (library.normalizedRectangles)
        warnings.push_back("Normalized " + std::to_string(library.normalizedRectangles)
            + " reversed LEF rectangle corner pairs in memory; original input files unchanged.");
    for (std::size_t i = 0; i < result.routes.size(); ++i) {
        const auto& r = result.routes[i];
        if (!r.success) { ++failed; failures.push_back(design.nets[i].name + ": " + r.error); continue; }
        ++routed;
        objective += resources.staticCost(r);
        if (!r.terminals.empty()) {
            auto p = r.terminals.front().point;
            router::Rect box{p.x, p.y, p.x, p.y};
            for (const auto& t : r.terminals) {
                box.x1 = std::min(box.x1, t.point.x); box.x2 = std::max(box.x2, t.point.x);
                box.y1 = std::min(box.y1, t.point.y); box.y2 = std::max(box.y2, t.point.y);
            }
            hpwl += static_cast<double>(box.x2 - box.x1 + box.y2 - box.y1) / grid.dbu();
        }
        for (const auto& edge : resources.routeEdges(r)) {
            const auto c = grid.cell(edge.first), d = grid.cell(edge.second);
            if (c.layer != d.layer) { if (resources.isCross(c.layer, d.layer)) ++cross; else ++vias; }
            else {
                const auto p = grid.center(c), q = grid.center(d);
                planar += static_cast<double>(std::abs(p.x - q.x) + std::abs(p.y - q.y)) / grid.dbu();
            }
        }
    }
    warnings.push_back("Local congestion omits fixed pin/OBS track demand and extra TSV endpoint demand; use the contest scorer for scores.");
    warnings.push_back("Guide connectivity does not certify detailed-routing DRC or pin-access geometry.");
    if (a.thermal.empty()) warnings.push_back("No thermal input: thermal optimization is disabled.");
    if (resources.crossViaNames().empty()) warnings.push_back("No cross-die VIA identified; use --cross-via for your technology.");
    if (result.timeLimitReached) warnings.push_back("Routing time limit reached; best available result retained.");
    const bool pass = failed == 0 && validation.passed;
    std::ostringstream out;
    out << std::setprecision(12) << "{\n  \"schema_version\": 1,\n  \"solver\": \"contest_router-1.4\",\n"
        << "  \"status\": " << quote(pass ? "PASS_LOCAL_GUIDE_CHECK" : "INCOMPLETE_OR_INVALID") << ",\n"
        << "  \"official_score\": null,\n  \"design\": " << quote(design.name) << ",\n"
        << "  \"inputs\": {\"lef\": " << strings(a.lefs) << ", \"def\": " << quote(a.def) << ", \"thermal\": " << quote(a.thermal) << "},\n"
        << "  \"parameters\": {\"iterations\": " << a.routing.iterations << ", \"time_limit_seconds\": " << a.routing.timeLimitSeconds
        << ", \"astar_weight\": " << a.routing.astarWeight
        << ", \"order\": " << quote(a.routing.inputOrder ? "def" : "short") << ", \"via_cost\": " << a.grid.viaCost
        << ", \"cross_via_cost\": " << a.resources.crossViaCost << ", \"thermal_weight\": " << a.resources.thermalWeight
        << ", \"thermal_search_radius\": " << a.routing.thermalSearchRadius
        << ", \"tsv_route_trials\": " << a.routing.tsvRouteTrials
        << ", \"present_weight\": " << a.resources.presentWeight << ", \"history_weight\": " << a.resources.historyWeight
        << ", \"capacity_scale\": " << a.resources.capacityScale
        << ", \"max_grid_nodes\": " << a.grid.maxNodes << ", \"grid_memory_mib\": " << a.grid.denseMemoryMiB
        << ", \"thermal_coordinates\": " << quote(a.resources.thermalCoordinates)
        << ", \"thermal_boundary_tolerance_dbu\": " << a.resources.thermalBoundaryToleranceDbu << "},\n"
        << "  \"grid\": {\"width\": " << grid.width() << ", \"height\": " << grid.height() << ", \"layers\": " << grid.layerCount()
        << ", \"nodes\": " << grid.size() << ", \"dbu_per_micron\": " << grid.dbu() << "},\n"
        << "  \"nets\": {\"total\": " << design.nets.size() << ", \"routed\": " << routed << ", \"failed\": " << failed << ", \"errors\": " << strings(failures) << "},\n"
        << "  \"input_compatibility\": {\"normalized_lef_rectangles\": " << library.normalizedRectangles << "},\n"
        << "  \"pin_access\": {\"escape_terminals\": " << escapedTerminals << "},\n"
        << "  \"wirelength\": {\"model\": \"unique GCell-center edges including escape cells; subcell pin lengths excluded; via costs are configurable proxies\""
        << ", \"planar_um\": " << planar << ", \"same_die_vias\": " << vias << ", \"cross_die_vias\": " << cross
        << ", \"equivalent_um\": " << planar + vias * a.grid.viaCost + cross * a.resources.crossViaCost
        << ", \"pin_hpwl_um\": " << hpwl << ", \"static_objective\": " << objective << "},\n"
        << "  \"congestion\": {\"model\": \"unique net per layer-GCell / preferred-direction DEF TRACKS\", \"total_overflow\": " << resources.overflow()
        << ", \"max_overflow\": " << resources.maxOverflow() << ", \"overflow_cells\": " << resources.overflowCells()
        << ", \"used_cells\": " << resources.usedCells() << "},\n"
        << "  \"thermal\": {\"loaded\": " << (resources.thermal().loaded ? "true" : "false")
        << ", \"synthetic\": " << (resources.thermal().synthetic ? "true" : "false") << ", \"mapped_nodes\": " << resources.thermal().mappedNodes
        << ", \"sampled_cells\": " << resources.thermal().sampledCells << ", \"filled_cells\": " << resources.thermal().filledCells
        << ", \"clipped_nodes\": " << resources.thermal().clippedNodes << ", \"coordinates\": " << quote(resources.thermal().coordinates)
        << ", \"offset_x_dbu\": " << resources.thermal().offsetX << ", \"offset_y_dbu\": " << resources.thermal().offsetY
        << ", \"ambient\": " << resources.thermal().ambient << ", \"pearson_raw\": " << resources.correlation()
        << ", \"pearson_above_ambient\": " << resources.correlation(true) << ", \"cross_via_names\": " << strings(resources.crossViaNames()) << "},\n"
        << "  \"runtime\": {\"elapsed_seconds\": " << seconds << ", \"peak_rss_mb\": " << peakMemoryMB()
        << ", \"expanded_nodes\": " << result.expanded << ", \"time_limit_reached\": " << (result.timeLimitReached ? "true" : "false") << "},\n"
        << "  \"validation\": " << validationJson(validation) << ",\n  \"iterations\": [";
    for (std::size_t i = 0; i < result.iterations.size(); ++i) {
        const auto& row = result.iterations[i];
        if (i) out << ',';
        out << "\n    {\"index\": " << row.index << ", \"failed\": " << row.failed << ", \"overflow\": " << row.overflow
            << ", \"cross_vias\": " << row.crossVias << ", \"static_cost\": " << row.cost << ", \"correlation\": " << row.correlation
            << ", \"rerouted\": " << row.rerouted << ", \"expanded\": " << row.expanded << ", \"seconds\": " << row.seconds
            << ", \"selected\": " << (row.selected ? "true" : "false") << '}';
    }
    out << "\n  ],\n  \"warnings\": " << strings(warnings) << "\n}\n";
    return out.str();
}
}

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) { help(); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--version") { std::cout << "contest_router 1.4\n"; return 0; }
    try {
        const auto started = Clock::now();
        auto a = arguments(argc, argv);
        const auto library = loadLibrary(a.lefs);
        if (library.normalizedRectangles)
            std::cerr << "[warning] Normalized " << library.normalizedRectangles
                      << " reversed LEF rectangles in memory; input files unchanged\n";
        const auto design = router::parseDef(a.def);
        const auto pins = router::resolvePins(design, library);
        router::GCellGrid grid(design, library, a.grid);
        grid.addObstacles(pins.obstructions);
        grid.addObstacles(design.blockages);
        if (!a.check.empty()) {
            const auto v = rc::validateGuide(a.check, design, pins, grid, a.resources.crossVias);
            PendingFile report(a.report);
            writeText(report.temporary, "{\n  \"validation\": " + validationJson(v) + "\n}\n");
            report.publish();
            for (const auto& error : v.errors) std::cerr << "[invalid] " << error << '\n';
            std::cout << (v.passed ? "PASS" : "FAIL") << " local guide check, nets=" << v.checkedNets << "\nReport: " << a.report << '\n';
            return v.passed ? 0 : 2;
        }
        if (a.thermal.empty()) a.resources.thermalWeight = 0;
        rc::Resources resources(design, grid, a.resources);
        if (!a.thermal.empty()) resources.loadThermal(a.thermal);
        std::cout << "Design: " << design.name << " | grid " << grid.width() << 'x' << grid.height() << 'x' << grid.layerCount()
                  << " | nets " << design.nets.size() << '\n';
        for (const auto& warning : resources.warnings()) std::cerr << "[warning] " << warning << '\n';
        auto remaining = a.routing;
        remaining.netProgress=[](std::size_t done,std::size_t total,double seconds) {
            std::cerr << "[routing] initial processed=" << done << '/' << total << " elapsed=" << seconds << "s\n";
        };
        remaining.timeLimitSeconds = std::max(0.0, a.routing.timeLimitSeconds
            - std::chrono::duration<double>(Clock::now()-started).count());
        const auto result = rc::route(design, pins, grid, resources, remaining, [](const rc::Iteration& r) {
            std::cout << "round=" << r.index << " failed=" << r.failed << " overflow=" << r.overflow
                      << " cross=" << r.crossVias << " cost=" << r.cost << " expanded=" << r.expanded
                      << (r.selected ? " best" : "") << '\n';
        });
        PendingFile guide(a.out), report(a.report);
        rc::writeGuide(guide.temporary, design, grid, resources, result.routes);
        const auto validation = rc::validateGuide(guide.temporary, design, pins, grid, resources.crossViaNames());
        const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
        writeText(report.temporary, reportJson(a, design, grid, resources, result, validation, seconds, library));
        guide.publish();
        report.publish();
        for (const auto& error : validation.errors) std::cerr << "[invalid] " << error << '\n';
        const bool all = std::all_of(result.routes.begin(), result.routes.end(), [](const auto& r) { return r.success; });
        std::cout << (all && validation.passed ? "PASS" : "FAIL") << " local guide check | overflow=" << resources.overflow()
                  << " | elapsed=" << seconds << "s\nGuide: " << a.out << "\nReport: " << a.report << '\n';
        return all && validation.passed ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
