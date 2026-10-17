#include "router/contest_resources.hpp"
#include "router/contest_topology.hpp"
#include "router/def_parser.hpp"
#include "router/gcell_grid.hpp"
#include "router/lef_parser.hpp"
#include "router/net_router.hpp"
#include "router/pin_resolver.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Diagnostic tool: replays the production pin-access path (chooseTerminal plus
// the same capacity / allowed-edge gate as terminalFor) for a list of pins and
// reports the first concrete reason each one fails. It never routes.
namespace {

std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (const char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            } else out += ch;
        }
    }
    return out + "\"";
}

struct Access {
    bool found = false;
    std::string reason;
    router::Point point{};
    router::Cell cell{};
    std::vector<int> accessLayers;
    std::string shapeNotes;
    bool partialObstruction = false;
};

// EXPERIMENT helper: is the cell blocked, and if so is it fully covered by a
// single obstruction rectangle (four corners inside it) rather than merely
// sampled at its center?
bool fullyCoveredCell(const router::GCellGrid& grid, const router::Cell& cell) {
    const auto p = grid.center(cell);
    if (!grid.blocked(cell)) return false;
    for (const auto& r : grid.obstacleRects(cell.layer))
        if (p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2
            && grid.xLines()[cell.x] >= r.x1 && grid.xLines()[cell.x + 1] <= r.x2
            && grid.yLines()[cell.y] >= r.y1 && grid.yLines()[cell.y + 1] <= r.y2)
            return true;
    return false;
}

// EXPERIMENT helper: same test as GCellGrid::addObstacles uses for blockedEdges.
bool edgeCrossesObstacle(const router::GCellGrid& grid, const router::Cell& a, const router::Cell& b) {
    const auto p = grid.center(a), q = grid.center(b);
    for (const auto& r : grid.obstacleRects(a.layer)) {
        if (p.y == q.y) {
            if (p.y >= r.y1 && p.y <= r.y2 && std::max(p.x, q.x) >= r.x1 && std::min(p.x, q.x) <= r.x2) return true;
        } else if (p.x >= r.x1 && p.x <= r.x2
                   && std::max(p.y, q.y) >= r.y1 && std::min(p.y, q.y) <= r.y2) return true;
    }
    return false;
}

// EXPERIMENT fallback: a pin rectangle inside a routing channel narrower than
// one GCell has its whole cell blocked by the center sample. Accept that access
// cell when the pin itself is clear of the obstruction and the cell is not fully
// covered; the escape edge is then left to the ordinary search.
bool partialAccess(const router::PinLocation& pin, const router::GCellGrid& grid,
                   const router::contest::Resources& resources, const router::contest::DieTopology& topology,
                   Access& result) {
    result.shapeNotes += " [partial-check:" + std::to_string(pin.shapes.size()) + " shapes]";
    for (const auto& shape : pin.shapes) {
        const int layer = grid.layerIndex(shape.layer);
        if (layer < 0) continue;
        const auto& r = shape.rect;
        const router::Point p{r.x1 + (r.x2 - r.x1) / 2, r.y1 + (r.y2 - r.y1) / 2};
        if (!grid.contains(p)) { result.shapeNotes += " [partial: point outside grid]"; continue; }
        if (grid.pointBlocked(p, layer)) { result.shapeNotes += " [partial: point inside OBS]"; continue; }
        const auto cell = grid.locate(p, layer);
        const auto mask = topology.mask(topology.die(layer));
        if (!mask.at(static_cast<std::size_t>(layer))) { result.shapeNotes += " [partial: layer not on this die]"; continue; }
        if (!grid.blocked(cell)) { result.shapeNotes += " [partial: cell not blocked]"; continue; }
        if (fullyCoveredCell(grid, cell)) { result.shapeNotes += " [partial: cell fully covered]"; continue; }
        if (resources.capacities().at(grid.id(cell)) <= 0) { result.shapeNotes += " [partial: zero capacity]"; continue; }
        result.found = true;
        result.partialObstruction = true;
        result.point = p;
        result.cell = cell;
        result.accessLayers = {layer};
        result.reason = "accepted through a partial obstruction (channel narrower than one GCell)";
        return true;
    }
    return false;
}

// EXPERIMENT helper: breadth-first escape from the partial access cell using the
// relaxed rule, to show whether the net can actually leave the macro border.
bool escapeFound(const router::GCellGrid& grid, const router::contest::Resources& resources,
                 router::Cell start, std::size_t limit = 200000) {
    std::vector<router::Cell> pending{start};
    std::unordered_set<int> seen{grid.id(start)};
    for (std::size_t i = 0; i < pending.size() && i < limit; ++i) {
        const auto current = pending[i];
        if (!grid.blocked(current)) return true;
        for (const auto& next : grid.neighbors(current)) {
            const int id = grid.id(next);
            if (!seen.insert(id).second) continue;
            if (!resources.allowed(current, next) && current.layer != next.layer) continue;
            pending.push_back(next);
        }
    }
    return false;
}

// Mirror of contest_router.cpp's usableTerminal / terminalFor, reporting the
// exact gate that rejects a candidate instead of a single summary exception.
Access evaluate(const router::PinLocation& pin, const router::GCellGrid& grid,
                const router::contest::Resources& resources, const router::contest::DieTopology& topology) {
    Access result;
    if (pin.shapes.empty()) { result.reason = "pin has no LEF/DEF shape"; return result; }
    bool sawLayer = false, sawInside = false, sawViaPath = false;
    bool sawCapacity = false, sawEdge = false, sawUnblocked = false;
    for (const auto& shape : pin.shapes) {
        const int layer = grid.layerIndex(shape.layer);
        const auto& r = shape.rect;
        const router::Point p{r.x1 + (r.x2 - r.x1) / 2, r.y1 + (r.y2 - r.y1) / 2};
        const bool inside = grid.contains(p);
        const bool pointBlocked = layer >= 0 && grid.pointBlocked(p, layer);
        if (!result.shapeNotes.empty()) result.shapeNotes += ' ';
        result.shapeNotes += shape.layer + "@(" + std::to_string(p.x) + "," + std::to_string(p.y) + ")"
                             + (layer < 0 ? " no-routing-layer" : inside ? "" : " outside-grid")
                             + (pointBlocked ? " point-blocked" : "");
        if (layer < 0) continue;
        sawLayer = true;
        const auto mask = topology.mask(topology.die(layer));
        const auto probeCell = grid.locate(p, layer);
        result.shapeNotes += " [cell-blocked=" + std::string(grid.blocked(probeCell) ? "1" : "0")
                           + " point-blocked=" + (grid.pointBlocked(p, layer) ? "1" : "0")
                           + " die=" + std::to_string(topology.die(layer))
                           + " mask-ok=" + (mask.at(static_cast<std::size_t>(layer)) ? "1" : "0") + "]";
        auto candidate = pin;
        candidate.shapes = {shape};
        Access attempt;
        try {
            const auto terminal = router::chooseTerminal(candidate, grid, topology.mask(topology.die(layer)));
            attempt.found = true;
            attempt.point = terminal.point;
            attempt.cell = terminal.cell;
            attempt.accessLayers = terminal.accessLayers;
        } catch (const std::exception& error) {
            result.shapeNotes += " [" + std::string(error.what()) + "]";
            continue;
        }
        sawInside = true;
        if (grid.blocked(attempt.cell) || grid.pointBlocked(attempt.point, attempt.cell.layer)) {
            // chooseTerminal already rejects blocked cells; keep as a safety net.
            continue;
        }
        sawUnblocked = true;
        sawViaPath = true;
        if (resources.capacities().at(grid.id(attempt.cell)) <= 0) continue;
        sawCapacity = true;
        bool edges = true;
        for (std::size_t i = 1; i < attempt.accessLayers.size(); ++i) {
            auto from = attempt.cell, to = attempt.cell;
            from.layer = attempt.accessLayers[i - 1];
            to.layer = attempt.accessLayers[i];
            if (!resources.allowed(from, to)) { edges = false; break; }
        }
        if (!edges) continue;
        sawEdge = true;
        return attempt;
    }
    if (!sawLayer) result.reason = "no pin shape on a routing layer";
    else if (!sawInside) result.reason = "chooseTerminal rejected every shape (grid bounds/blockage)";
    else if (!sawUnblocked) result.reason = "candidate GCell center/point blocked by obstacle";
    else if (!sawViaPath) result.reason = "no Via graph path to an unblocked layer";
    else if (!sawCapacity) result.reason = "candidate GCell capacity is zero";
    else if (!sawEdge) result.reason = "blocked/zero-capacity hop along the pin access layer chain";
    // EXPERIMENT: retry the access with the partial-obstruction rule.
    if (partialAccess(pin, grid, resources, topology, result)) return result;
    return result;
}

std::string layerNames(const router::GCellGrid& grid, const std::vector<int>& layers) {
    std::string out;
    for (const int layer : layers) {
        if (!out.empty()) out += '>';
        out += grid.layer(layer).name;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string lef, def, thermal, pinsFile, outFile;
        int limit = 40;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + key);
                return argv[++i];
            };
            if (key == "--lef") lef = value();
            else if (key == "--def") def = value();
            else if (key == "--thermal") thermal = value();
            else if (key == "--pins") pinsFile = value();
            else if (key == "--out") outFile = value();
            else if (key == "--limit") limit = std::stoi(value());
            else throw std::runtime_error("Unknown argument: " + key);
        }
        if (lef.empty() || def.empty() || pinsFile.empty() || outFile.empty())
            throw std::runtime_error("Usage: contest_pin_probe --lef a.lef --def a.def [--thermal t.json] --pins pins.txt --out report.json [--limit N]");

        std::vector<router::Connection> probes;
        {
            std::ifstream input(pinsFile);
            if (!input) throw std::runtime_error("Cannot read pins file: " + pinsFile);
            std::string line;
            while (std::getline(input, line)) {
                std::istringstream row(line);
                std::string instance, pin;
                if (!(row >> instance >> pin)) continue;
                if (instance.empty() || instance[0] == '#') continue;
                probes.push_back({instance, pin});
            }
        }

        const auto started = std::chrono::steady_clock::now();
        const auto library = router::parseLef(lef);
        const auto design = router::parseDef(def);
        const auto pins = router::resolvePins(design, library);
        router::GridOptions gridOptions;
        gridOptions.maxNodes = 50000000;
        router::GCellGrid grid(design, library, gridOptions);
        grid.addObstacles(pins.obstructions);
        grid.addObstacles(design.blockages);
        router::contest::ResourceOptions resourceOptions;
        router::contest::Resources resources(design, grid, resourceOptions);
        if (!thermal.empty()) resources.loadThermal(thermal);
        const router::contest::DieTopology topology(grid, resources.crossViaNames());
        // Compile-time proof of which grid rule set is linked into this binary.
        bool experimentalGrid = false;
        try { experimentalGrid = grid.usable(router::Cell{0, 0, 0}); } catch (const std::exception&) {}
        std::cerr << "[probe] grid rule: " << (experimentalGrid ? "partial-obstruction" : "center-sample-only") << '\n';

        std::size_t ok = 0, missing = 0, failed = 0, examined = 0, recoverable = 0, escapes = 0;
        std::map<std::string, std::size_t> reasons, failingLayers, failingInstances;
        std::vector<std::string> details;
        // Counter-check: can the pin cell be entered over an edge that does not
        // cross an obstruction, i.e. is only the cell center inside the OBS?
        const auto reachablePinCell = [&](const router::PinLocation& pin, const router::GCellGrid& grid,
                                          const router::contest::Resources& resources, std::string& note) {
            for (const auto& shape : pin.shapes) {
                const int layer = grid.layerIndex(shape.layer);
                if (layer < 0) continue;
                const auto& r = shape.rect;
                const router::Point p{r.x1 + (r.x2 - r.x1) / 2, r.y1 + (r.y2 - r.y1) / 2};
                if (!grid.contains(p) || grid.pointBlocked(p, layer)) continue;
                const auto cell = grid.locate(p, layer);
                if (!grid.blocked(cell)) continue;
                if (resources.capacities().at(grid.id(cell)) <= 0) continue;
                const auto cellCenter = grid.center(cell);
                std::string blockers;
                for (const auto& rect : grid.obstacleRects(layer)) {
                    const bool centerInside = cellCenter.x >= rect.x1 && cellCenter.x <= rect.x2
                                           && cellCenter.y >= rect.y1 && cellCenter.y <= rect.y2;
                    const bool pinInside = p.x >= rect.x1 && p.x <= rect.x2 && p.y >= rect.y1 && p.y <= rect.y2;
                    if (!centerInside && !pinInside) continue;
                    if (!blockers.empty()) blockers += ' ';
                    blockers += (centerInside ? "center-in:" : "pin-in:")
                              + std::to_string(rect.x1) + "," + std::to_string(rect.y1) + ","
                              + std::to_string(rect.x2) + "," + std::to_string(rect.y2);
                }
                if (blockers.size() > 220) blockers = blockers.substr(0, 220) + "...";
                note = shape.layer + " pin cell " + std::to_string(cell.x) + "," + std::to_string(cell.y)
                       + " center(" + std::to_string(cellCenter.x) + "," + std::to_string(cellCenter.y) + ")"
                       + " center-blocked, pin" + (grid.pointBlocked(p, layer) ? "-blocked" : "-free")
                       + " obstacles=" + (blockers.empty() ? std::string("none") : blockers);
                return true;
            }
            return false;
        };
        // Counter-check: does the pin geometry itself reach a free GCell even
        // though the rectangle-center cell is blocked by a macro obstruction?
        const auto freeCellInRect = [&](const router::PinLocation& pin, std::string& note) {
            for (const auto& shape : pin.shapes) {
                const int layer = grid.layerIndex(shape.layer);
                if (layer < 0) continue;
                const auto& r = shape.rect;
                const int x1 = std::max(0, grid.locate({r.x1, r.y1}, layer).x - 1);
                const int x2 = std::min(grid.width() - 1, grid.locate({r.x2, r.y2}, layer).x + 1);
                const int y1 = std::max(0, grid.locate({r.x1, r.y1}, layer).y - 1);
                const int y2 = std::min(grid.height() - 1, grid.locate({r.x2, r.y2}, layer).y + 1);
                for (int y = y1; y <= y2; ++y) for (int x = x1; x <= x2; ++x) {
                    const router::Cell cell{x, y, layer};
                    if (grid.blocked(cell) || resources.capacities().at(grid.id(cell)) <= 0) continue;
                    const auto c = grid.center(cell);
                    if (c.x >= r.x1 && c.x <= r.x2 && c.y >= r.y1 && c.y <= r.y2) {
                        note = shape.layer + " free-center cell " + std::to_string(x) + "," + std::to_string(y);
                        return true;
                    }
                }
            }
            return false;
        };
        for (const auto& connection : probes) {
            ++examined;
            const router::PinLocation* pin = nullptr;
            try {
                pin = &router::lookupPin(pins, connection);
            } catch (const std::exception&) {
                ++missing;
                reasons["pin not found in LEF/DEF"] += 1;
                continue;
            }
            const auto access = evaluate(*pin, grid, resources, topology);
            if (access.found) { ++ok; continue; }
            ++failed;
            reasons[access.reason] += 1;
            const std::string label = connection.instance + "/" + connection.pin;
            failingInstances[connection.instance] += 1;
            std::string shapes;
            for (const auto& shape : pin->shapes) {
                const int layer = grid.layerIndex(shape.layer);
                if (layer < 0) continue;
                failingLayers[shape.layer] += 1;
                if (!shapes.empty()) shapes += ';';
                shapes += shape.layer + ":" + std::to_string(shape.rect.x1) + "," + std::to_string(shape.rect.y1)
                        + "," + std::to_string(shape.rect.x2) + "," + std::to_string(shape.rect.y2);
            }
            // Counter-check: does the pin geometry itself reach a free GCell,
            // even though the rectangle-center cell is blocked by a macro OBS?
            std::string recovery;
            if (freeCellInRect(*pin, recovery)) ++recoverable;
            // Second counter-check: can the pin's own cell be entered over an
            // edge that does not cross an obstruction, i.e. is the blockage
            // only the cell-center sample rather than the whole pixel?
            std::string escape;
            if (reachablePinCell(*pin, grid, resources, escape)) ++escapes;
            if (details.size() < static_cast<std::size_t>(limit)) {
                std::ostringstream row;
                row << "    {\"pin\": " << jsonString(label)
                    << ", \"master\": " << jsonString(pin->master)
                    << ", \"use\": " << jsonString(pin->use)
                    << ", \"reason\": " << jsonString(access.reason)
                    << ", \"recovery\": " << jsonString(recovery)
                    << ", \"escape\": " << jsonString(escape)
                    << ", \"shape_notes\": " << jsonString(access.shapeNotes)
                    << ", \"shapes\": " << jsonString(shapes) << '}';
                details.push_back(row.str());
            }
        }

        std::ostringstream out;
        out << "{\n  \"scope\": \"pin access replay using production chooseTerminal; no routing\",\n"
            << "  \"design\": " << jsonString(design.name) << ", \"grid\": \"" << grid.width() << 'x' << grid.height()
            << 'x' << grid.layerCount() << "\", \"cells\": \"" << grid.xLines()[1] - grid.xLines()[0] << 'x'
            << grid.yLines()[1] - grid.yLines()[0] << " dbu\",\n"
            << "  \"die\": {\"x1\": " << design.die.x1 << ", \"y1\": " << design.die.y1
            << ", \"x2\": " << design.die.x2 << ", \"y2\": " << design.die.y2 << "},\n"
            << "  \"grid_bounds\": {\"x1\": " << grid.xLines().front() << ", \"x2\": " << grid.xLines().back()
            << ", \"y1\": " << grid.yLines().front() << ", \"y2\": " << grid.yLines().back()
            << ", \"x_lines\": " << grid.xLines().size() << ", \"y_lines\": " << grid.yLines().size() << "},\n"
            << "  \"routing_layers\": " << grid.layerCount() << ",\n"
            << "  \"probed\": " << examined << ", \"access_ok\": " << ok << ", \"access_failed\": " << failed
            << ", \"pin_missing\": " << missing << ", \"recoverable_via_free_in_shape_cell\": " << recoverable
            << ", \"center_blocked_but_pin_cell_has_free_planar_neighbour\": " << escapes
            << ",\n  \"reasons\": {";
        bool first = true;
        for (const auto& item : reasons) {
            out << (first ? "\n" : ",\n") << "    " << jsonString(item.first) << ": " << item.second;
            first = false;
        }
        out << (first ? "" : "\n  ") << "},\n  \"failed_shape_layers\": {";
        first = true;
        for (const auto& item : failingLayers) {
            out << (first ? "\n" : ",\n") << "    " << jsonString(item.first) << ": " << item.second;
            first = false;
        }
        out << (first ? "" : "\n  ") << "},\n  \"failed_instances\": {";
        first = true;
        for (const auto& item : failingInstances) {
            out << (first ? "\n" : ",\n") << "    " << jsonString(item.first) << ": " << item.second;
            first = false;
        }
        out << (first ? "" : "\n  ") << "},\n  \"first_failures\": [\n";
        for (std::size_t i = 0; i < details.size(); ++i) out << details[i] << (i + 1 == details.size() ? "\n" : ",\n");
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        out << "  ],\n  \"elapsed_seconds\": " << seconds << "\n}\n";

        std::ofstream report(outFile, std::ios::binary | std::ios::trunc);
        if (!report || !(report << out.str()) || !report.flush())
            throw std::runtime_error("Cannot write report: " + outFile);
        std::cout << "probed=" << examined << " access_ok=" << ok << " access_failed=" << failed
                  << " pin_missing=" << missing << " elapsed=" << seconds << "s\n";
        for (const auto& item : reasons) std::cout << "  " << item.second << "\t" << item.first << '\n';
        (void)layerNames;
        return failed ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
