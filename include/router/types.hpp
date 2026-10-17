#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace router {

// LEF 中的距离单位是微米，DEF 中的整数坐标单位是 DBU。
using Coord = std::int64_t;
struct Point { Coord x = 0, y = 0; };
struct Rect { Coord x1 = 0, y1 = 0, x2 = 0, y2 = 0; };
struct LefRect {
    std::string layer;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};
struct Layer {
    std::string name, type, direction;
    double width = 0, pitch = 0;
};
struct LefPin {
    std::string name, use = "SIGNAL";
    std::vector<LefRect> shapes;
};
struct Macro {
    std::string name, cellClass;
    double width = 0, height = 0, originX = 0, originY = 0;
    std::map<std::string, LefPin> pins;
    std::vector<LefRect> obstructions;
};
struct Via {
    std::string name;
    bool isDefault = false;
    std::vector<std::string> layers;
};
struct Library {
    std::vector<Layer> layers;
    std::map<std::string, Macro> macros;
    std::vector<Via> vias;
    std::size_t normalizedRectangles = 0; // Reversed LEF corner pairs normalized in memory.
};
struct Instance {
    std::string name, master, orientation = "N";
    Point position;
    bool placed = false;
};
struct PinShape { std::string layer; Rect rect; std::string obstructionOwner{}; };
struct DefPort {
    std::vector<PinShape> shapes; // 相对于 PORT 的放置原点。
    Point position;
    std::string orientation = "N";
    bool placed = false;
};
struct DefPin {
    std::string name, net, use = "SIGNAL";
    std::vector<DefPort> ports;
};
struct GridPattern {
    char axis = 'X';
    Coord start = 0, step = 0;
    int count = 0; // DO 的数量是网格线数量，不是格子数量。
};
struct Connection { std::string instance, pin; };
struct Span { std::size_t begin = 0, end = 0; };
struct Net {
    std::string name, use = "SIGNAL";
    std::vector<Connection> connections;
    Span source;                  // 原始 DEF 中整条 net 的范围。
    std::vector<Span> oldRoutes;  // 只替换原有的布线属性。
};
struct Design {
    std::string name, source;
    Coord dbuPerMicron = 0;
    Rect die;
    std::vector<GridPattern> gridPatterns;
    std::map<std::string, Instance> instances;
    std::map<std::string, DefPin> pins;
    std::vector<Net> nets;
    std::vector<PinShape> blockages; // 仅 routing RECT blockage。
};
struct PinLocation {
    std::string instance, master, pin, use;
    std::vector<PinShape> shapes; // 已转换为 DEF 绝对坐标。
};
struct PinDatabase {
    std::map<std::string, std::map<std::string, PinLocation>> instances;
    std::map<std::string, PinLocation> ports;
    std::vector<PinShape> obstructions;
};
struct Cell {
    int x = 0, y = 0, layer = 0;
    bool operator==(const Cell& b) const {
        return x == b.x && y == b.y && layer == b.layer;
    }
};
struct Terminal {
    Point point;
    Cell cell;
    std::vector<int> accessLayers; // 在真实 pin 坐标处换层，再接入 GCELL 中心。
    std::vector<Cell> accessPath{}; // Pin-specific escape cells; never unblock the global grid.
};
struct RoutedNet {
    bool success = false, skipped = false;
    std::string error;
    std::vector<Terminal> terminals;
    std::vector<std::vector<Cell>> branches;
    std::size_t expanded = 0;
};

} // namespace router
