#include "router/contest_guide.hpp"
#include "router/pin_resolver.hpp"
#include "router/contest_topology.hpp"
#include <array>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace router::contest {
namespace {

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

bool guideName(const std::string& name) {
    return !name.empty() && name == trim(name) && name != "(" && name != ")"
           && name.find_first_of("\r\n") == std::string::npos;
}

bool coordinate(const std::string& text, Coord& value) {
    if (text.empty()) return false;
    // Accept an explicit positive sign as well as the usual integer DBU spelling.
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    if (*begin == '+') {
        ++begin;
        if (begin == end || *begin == '-' || *begin == '+') return false;
    }
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

int lineIndex(const std::vector<Coord>& lines, Coord value) {
    const auto found = std::lower_bound(lines.begin(), lines.end(), value);
    return found != lines.end() && *found == value
               ? static_cast<int>(found - lines.begin()) : -1;
}

struct Region {
    Rect rect;
    int layer = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

bool overlaps(Rect a, Rect b) {
    // A boundary-only touch does not establish physical pin access.
    return std::max(a.x1, b.x1) < std::min(a.x2, b.x2)
           && std::max(a.y1, b.y1) < std::min(a.y2, b.y2);
}

void error(Validation& result, const std::string& message) {
    result.passed = false;
    result.errors.push_back(message);
}

void checkRegions(const Net& net, const std::vector<Region>& regions,
                  const PinDatabase& pins, const GCellGrid& grid, const DieTopology& topology, Validation& result) {
    const auto label = "Net " + net.name + ": ";
    std::unordered_set<int> cells;
    for (const auto& region : regions)
        for (int y = region.y1; y < region.y2; ++y)
            for (int x = region.x1; x < region.x2; ++x)
                cells.insert(grid.id({x, y, region.layer}));

    if (!cells.empty()) {
        // Validate the entire guide union, including spurious disconnected islands.
        // Do not use grid.neighbors(): its center-based obstacle approximation can
        // exclude a legitimate pin access cell. This is guide validation, not DRC.
        std::vector<int> pending{*cells.begin()};
        cells.erase(pending.front());
        const auto visit = [&](Cell next) {
            if (grid.valid(next) && cells.erase(grid.id(next)) != 0)
                pending.push_back(grid.id(next));
        };
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto current = grid.cell(pending[i]);
            visit({current.x + 1, current.y, current.layer});
            visit({current.x - 1, current.y, current.layer});
            visit({current.x, current.y + 1, current.layer});
            visit({current.x, current.y - 1, current.layer});
            for (const auto& link : grid.viaLinks(current.layer))
                visit({current.x, current.y, link.layer});
        }
        if (!cells.empty()) error(result, label + "guide regions are disconnected");
    }

    std::set<int> pinDies;
    for (const auto& connection : net.connections) {
        try {
            const auto& pin = lookupPin(pins, connection);
            bool covered = false;
            for (const auto& shape : pin.shapes) {
                const auto layer = grid.layerIndex(shape.layer);
                if (layer < 0) continue;
                for (const auto& region : regions)
                    if (region.layer == layer && overlaps(region.rect, shape.rect)) {
                        covered = true;
                        // Classify the actual access shapes. Unused alternate
                        // shapes must not make the result depend on LEF order.
                        pinDies.insert(topology.die(layer));
                        break;
                    }
            }
            if (!covered)
                error(result, label + "pin " + connection.instance + "/" + connection.pin
                                  + " is not covered on an actual routing-layer shape");
        } catch (const std::exception& exception) {
            error(result, label + exception.what());
        }
    }
    if (pinDies.size() == 1) {
        for (const auto& region : regions) if (topology.die(region.layer) != *pinDies.begin()) {
            error(result, label + "same-die net uses layers belonging to another die"); break;
        }
    } else if (pinDies.size() > 1) {
        std::set<std::array<Coord,4>> candidates;
        for (const auto& pair : topology.crossings()) {
            if (!pinDies.count(topology.die(pair.first)) || !pinDies.count(topology.die(pair.second))) continue;
            for (const auto& a : regions) if (a.layer == pair.first)
                for (const auto& b : regions) if (b.layer == pair.second && overlaps(a.rect,b.rect))
                    candidates.insert({std::max(a.rect.x1,b.rect.x1), std::max(a.rect.y1,b.rect.y1),
                                       std::min(a.rect.x2,b.rect.x2), std::min(a.rect.y2,b.rect.y2)});
        }
        // Our construction promises a unique landing. Deliberately stricter
        // than the scorer's special two-candidate pin-based disambiguation.
        if (candidates.size() != 1)
            error(result, label + "local unique-TSV policy requires exactly one overlap candidate; found "
                          + std::to_string(candidates.size()));
    }
}

} // namespace

void writeGuide(const std::string& path, const Design& design, const GCellGrid& grid,
                const Resources& resources, const std::vector<RoutedNet>& routes) {
    if (routes.size() != design.nets.size())
        throw std::runtime_error("Guide route count does not match DEF NETS");
    std::unordered_set<std::string> names;
    for (const auto& net : design.nets) {
        if (!guideName(net.name)) throw std::runtime_error("Invalid guide net name: " + net.name);
        if (!names.insert(net.name).second)
            throw std::runtime_error("Duplicate DEF net name: " + net.name);
    }
    // Always emit LF bytes, including on Windows. Linux EDA guide readers can
    // treat a CR as part of the net name rather than as a line terminator.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write guide: " + path);
    for (std::size_t i = 0; i < design.nets.size(); ++i) {
        out << design.nets[i].name << "\n(\n";
        if (routes[i].success) {
            // id order is (layer, y, x); merge only consecutive cells within a row.
            // This includes every pin access layer and never fills a hole or an
            // extra row just to make the rectangles larger.
            auto nodes = resources.routeNodes(routes[i]);
            std::sort(nodes.begin(), nodes.end());
            nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
            for (std::size_t begin = 0; begin < nodes.size();) {
                const auto first = grid.cell(nodes[begin]);
                auto last = first;
                auto end = begin + 1;
                while (end < nodes.size()) {
                    const auto next = grid.cell(nodes[end]);
                    if (next.layer != first.layer || next.y != first.y || next.x != last.x + 1)
                        break;
                    last = next;
                    ++end;
                }
                out << grid.xLines()[first.x] << ' ' << grid.yLines()[first.y] << ' '
                    << grid.xLines()[last.x + 1] << ' ' << grid.yLines()[first.y + 1]
                    << ' ' << grid.layer(first.layer).name << '\n';
                begin = end;
            }
        }
        out << ")\n";
    }
    out.flush();
    if (!out) throw std::runtime_error("Failed to write guide: " + path);
}

Validation validateGuide(const std::string& path, const Design& design,
                         const PinDatabase& pins, const GCellGrid& grid, const std::set<std::string>& crossViaNames) {
    Validation result;
    const DieTopology topology(grid,crossViaNames);
    std::map<std::string, const Net*> nets;
    for (const auto& net : design.nets)
        if (!nets.emplace(net.name, &net).second)
            error(result, "Duplicate DEF net name: " + net.name);

    std::ifstream input(path);
    if (!input) {
        error(result, "Cannot read guide: " + path);
        return result;
    }
    std::size_t lineNumber = 0;
    std::string line;
    const auto nextLine = [&]() {
        while (std::getline(input, line)) {
            ++lineNumber;
            line = trim(line);
            if (!line.empty()) return true;
        }
        return false;
    };
    std::unordered_set<std::string> seen;
    while (nextLine()) {
        const auto name = line;
        const auto headerLine = lineNumber;
        const auto found = nets.find(name);
        const bool unique = seen.insert(name).second;
        const auto context = [&]() {
            return "Guide line " + std::to_string(lineNumber) + " (net " + name + "): ";
        };
        if (!guideName(name)) {
            error(result, context() + "expected a net name");
            break;
        }
        if (found == nets.end()) error(result, context() + "unknown net");
        if (!unique) error(result, context() + "duplicate net");
        if (!nextLine() || line != "(") {
            error(result, "Guide net " + name + " at line " + std::to_string(headerLine)
                              + ": expected '(' on the following line");
            break;
        }

        std::vector<Region> regions;
        bool closed = false;
        while (nextLine()) {
            if (line == ")") { closed = true; break; }
            std::istringstream row(line);
            std::string values[4], layerName, extra;
            Region region;
            if (!(row >> values[0] >> values[1] >> values[2] >> values[3] >> layerName)
                || (row >> extra)) {
                error(result, context() + "expected 'x1 y1 x2 y2 routing_layer'");
                continue;
            }
            if (!coordinate(values[0], region.rect.x1) || !coordinate(values[1], region.rect.y1)
                || !coordinate(values[2], region.rect.x2) || !coordinate(values[3], region.rect.y2)) {
                error(result, context() + "coordinates must be representable integer DBU values");
                continue;
            }
            ++result.rectangles;
            region.layer = grid.layerIndex(layerName);
            if (region.layer < 0) {
                error(result, context() + "unknown routing layer: " + layerName);
                continue;
            }
            const auto r = region.rect;
            if (r.x1 >= r.x2 || r.y1 >= r.y2) {
                error(result, context() + "rectangle must have positive area");
                continue;
            }
            if (r.x1 < design.die.x1 || r.y1 < design.die.y1
                || r.x2 > design.die.x2 || r.y2 > design.die.y2) {
                error(result, context() + "rectangle lies outside DIEAREA");
                continue;
            }
            region.x1 = lineIndex(grid.xLines(), r.x1);
            region.y1 = lineIndex(grid.yLines(), r.y1);
            region.x2 = lineIndex(grid.xLines(), r.x2);
            region.y2 = lineIndex(grid.yLines(), r.y2);
            if (region.x1 < 0 || region.y1 < 0 || region.x2 < 0 || region.y2 < 0) {
                error(result, context() + "rectangle boundaries are not GCELLGRID aligned");
                continue;
            }
            regions.push_back(region);
        }
        if (!closed) error(result, "Guide net " + name + ": missing closing ')'");
        if (found != nets.end() && unique) {
            ++result.checkedNets;
            checkRegions(*found->second, regions, pins, grid, topology, result);
        }
        if (!closed) break;
    }
    if (input.bad()) error(result, "I/O failure reading guide: " + path);
    for (const auto& item : nets)
        if (seen.count(item.first) == 0) error(result, "Missing guide net: " + item.first);
    return result;
}

} // namespace router::contest
