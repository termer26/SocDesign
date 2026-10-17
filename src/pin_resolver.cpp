#include "router/pin_resolver.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace router {

Point rotatePoint(Point p, const std::string& orientation) {
    if (orientation == "N")  return { p.x,  p.y};
    if (orientation == "S")  return {-p.x, -p.y};
    if (orientation == "W")  return {-p.y,  p.x}; // 逆时针 90 度。
    if (orientation == "E")  return { p.y, -p.x};
    if (orientation == "FN") return {-p.x,  p.y};
    if (orientation == "FS") return { p.x, -p.y};
    if (orientation == "FW") return { p.y,  p.x};
    if (orientation == "FE") return {-p.y, -p.x};
    throw std::runtime_error("Unknown DEF orientation: " + orientation);
}

Point placeMacroPoint(Point p, Coord width, Coord height,
                      Point position, const std::string& orientation) {
    const auto q = rotatePoint(p, orientation);
    const auto a = rotatePoint({0, 0}, orientation);
    const auto b = rotatePoint({width, 0}, orientation);
    const auto c = rotatePoint({0, height}, orientation);
    const auto d = rotatePoint({width, height}, orientation);
    const Coord minX = std::min({a.x, b.x, c.x, d.x});
    const Coord minY = std::min({a.y, b.y, c.y, d.y});
    return {position.x + q.x - minX, position.y + q.y - minY};
}

namespace {
template <class Transform>
Rect transformRect(Rect r, Transform transform) {
    const std::array<Point, 4> points = {
        transform(Point{r.x1, r.y1}), transform(Point{r.x2, r.y1}),
        transform(Point{r.x1, r.y2}), transform(Point{r.x2, r.y2})};
    Rect out{points[0].x, points[0].y, points[0].x, points[0].y};
    for (auto p : points) {
        out.x1 = std::min(out.x1, p.x); out.x2 = std::max(out.x2, p.x);
        out.y1 = std::min(out.y1, p.y); out.y2 = std::max(out.y2, p.y);
    }
    return out;
}
PinShape absoluteShape(const LefRect& r, const Macro& macro,
                       const Instance& inst, Coord dbu) {
    const auto unit = [dbu](double um) { return static_cast<Coord>(std::llround(um * dbu)); };
    // LEF ORIGIN 是几何平移量，先平移到单元外框坐标，再应用朝向。
    Rect local{unit(r.x1 + macro.originX), unit(r.y1 + macro.originY),
               unit(r.x2 + macro.originX), unit(r.y2 + macro.originY)};
    const auto rect = transformRect(local, [&](Point p) {
        return placeMacroPoint(p, unit(macro.width), unit(macro.height),
                               inst.position, inst.orientation);
    });
    return {r.layer, rect};
}
std::string csv(const std::string& value) {
    std::string result = "\"";
    for (char ch : value) { result += ch; if (ch == '"') result += '"'; }
    return result + "\"";
}
} // namespace

PinDatabase resolvePins(const Design& design, const Library& library) {
    PinDatabase result;
    for (const auto& item : design.instances) {
        const auto& inst = item.second;
        const auto found = library.macros.find(inst.master);
        if (found == library.macros.end()) throw std::runtime_error("Missing LEF master: " + inst.master);
        if (!inst.placed) throw std::runtime_error("Unplaced instance: " + inst.name);
        const auto& macro = found->second;
        auto& pinMap = result.instances[inst.name];
        for (const auto& entry : macro.pins) {
            const auto& pin = entry.second;
            PinLocation location{inst.name, inst.master, pin.name, pin.use, {}};
            for (const auto& r : pin.shapes)
                location.shapes.push_back(absoluteShape(r, macro, inst, design.dbuPerMicron));
            pinMap.emplace(pin.name, location);
        }
        // CORE 单元的细小 OBS 属于详细布线资源，不能把整个 GCELL 封死。
        // 本全局模型只把宏单元/其它非 CORE 单元 OBS 作为硬障碍。
        if (macro.cellClass != "CORE")
            for (const auto& r : macro.obstructions) {
                auto shape=absoluteShape(r, macro, inst, design.dbuPerMicron);
                shape.obstructionOwner=inst.name;
                result.obstructions.push_back(std::move(shape));
            }
    }
    for (const auto& item : design.pins) {
        const auto& pin = item.second;
        PinLocation location{"PIN", "", pin.name, pin.use, {}};
        for (const auto& port : pin.ports) {
            if (!port.placed) throw std::runtime_error("Unplaced DEF PORT: " + pin.name);
            for (const auto& shape : port.shapes) {
                const auto r = transformRect(shape.rect, [&](Point p) {
                    const auto q = rotatePoint(p, port.orientation);
                    return Point{port.position.x + q.x, port.position.y + q.y};
                });
                location.shapes.push_back({shape.layer, r});
            }
        }
        result.ports.emplace(pin.name, location);
    }
    return result;
}

const PinLocation& lookupPin(const PinDatabase& pins, const Connection& connection) {
    if (connection.instance == "PIN") {
        const auto it = pins.ports.find(connection.pin);
        if (it != pins.ports.end()) return it->second;
    } else {
        const auto inst = pins.instances.find(connection.instance);
        if (inst != pins.instances.end()) {
            const auto pin = inst->second.find(connection.pin);
            if (pin != inst->second.end()) return pin->second;
        }
    }
    throw std::runtime_error("Missing pin: " + connection.instance + "/" + connection.pin);
}

void writePinsCsv(const std::string& path, const PinDatabase& pins) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write pin CSV: " + path);
    out << "kind,instance,master,pin,use,shape,layer,x1_dbu,y1_dbu,x2_dbu,y2_dbu,center_x_dbu,center_y_dbu\n";
    const auto write = [&](const PinLocation& p, const std::string& kind) {
        for (std::size_t i = 0; i < p.shapes.size(); ++i) {
            const auto& s = p.shapes[i]; const auto& r = s.rect;
            out << kind << ',' << csv(p.instance) << ',' << csv(p.master) << ','
                << csv(p.pin) << ',' << csv(p.use) << ',' << i << ',' << csv(s.layer)
                << ',' << r.x1 << ',' << r.y1 << ',' << r.x2 << ',' << r.y2
                << ',' << r.x1 + (r.x2 - r.x1) / 2 << ',' << r.y1 + (r.y2 - r.y1) / 2 << '\n';
        }
    };
    for (const auto& inst : pins.instances)
        for (const auto& pin : inst.second) write(pin.second, "INSTANCE");
    for (const auto& pin : pins.ports) write(pin.second, "PORT");
    if (!out) throw std::runtime_error("Failed to write pin CSV: " + path);
}

} // namespace router
