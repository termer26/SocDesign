#include "router/def_parser.hpp"
#include "router/tokenizer.hpp"
#include <algorithm>
#include <limits>

namespace router {
namespace {

Point point(Cursor& c) {
    c.expect("(");
    Point p;
    p.x = integer(c.take().text);
    p.y = integer(c.take().text);
    c.expect(")");
    return p;
}
Rect rectangle(Cursor& c) {
    const auto a = point(c), b = point(c);
    if (a.x >= b.x || a.y >= b.y) throw std::runtime_error("Invalid DEF rectangle");
    return {a.x, a.y, b.x, b.y};
}

template <class ParseRecord>
void section(Cursor& c, const std::string& name, ParseRecord parse) {
    const auto expected = integer(c.take().text);
    c.expect(";");
    Coord count = 0;
    while (!c.accept("END")) {
        const auto dash = c.take();
        if (dash.text != "-") throw std::runtime_error("Expected '-' in " + name);
        auto record = c.statement();
        record.insert(record.begin(), dash);
        parse(record);
        ++count;
    }
    c.expect(name);
    if (count != expected) throw std::runtime_error(name + " count does not match header");
}

Instance component(const std::vector<Token>& tokens) {
    Cursor c(tokens);
    c.expect("-");
    Instance inst;
    inst.name = c.take().text;
    inst.master = c.take().text;
    while (!c.empty()) {
        if (!c.accept("+")) { c.take(); continue; }
        const auto key = c.take().text;
        if (key == "PLACED" || key == "FIXED" || key == "COVER") {
            inst.position = point(c);
            inst.orientation = c.take().text;
            inst.placed = true;
        }
    }
    return inst;
}

DefPin ioPin(const std::vector<Token>& tokens) {
    Cursor c(tokens);
    c.expect("-");
    DefPin pin;
    pin.name = c.take().text;
    pin.ports.emplace_back();
    bool explicitPort = false;
    while (!c.empty()) {
        if (!c.accept("+")) { c.take(); continue; }
        const auto key = c.take().text;
        if (key == "NET") pin.net = c.take().text;
        else if (key == "USE") pin.use = c.take().text;
        else if (key == "PORT") {
            if (explicitPort || !pin.ports.back().shapes.empty() || pin.ports.back().placed)
                pin.ports.emplace_back();
            explicitPort = true;
        } else if (key == "LAYER") {
            const auto layer = c.take().text;
            if (c.accept("MASK")) c.take();
            if (c.peek().text != "(")
                throw std::runtime_error("Unsupported DEF pin LAYER options: " + pin.name);
            pin.ports.back().shapes.push_back({layer, rectangle(c)});
        } else if (key == "PLACED" || key == "FIXED" || key == "COVER") {
            auto& port = pin.ports.back();
            port.position = point(c);
            port.orientation = c.take().text;
            port.placed = true;
        } else if (key == "POLYGON" || key == "VIA") {
            throw std::runtime_error("Unsupported DEF pin geometry: " + key);
        }
    }
    return pin;
}

Net netRecord(const std::vector<Token>& tokens) {
    Cursor c(tokens);
    c.expect("-");
    Net net;
    net.source = {tokens.front().begin, tokens.back().end};
    net.name = c.take().text;
    if (net.name == "(") throw std::runtime_error("DEF MUSTJOIN nets are not supported");
    // 连接列表只在属性 '+' 之前解析，避免把 ROUTED 坐标当成连接。
    while (c.accept("(")) {
        Connection conn;
        conn.instance = c.take().text;
        conn.pin = c.take().text;
        if (c.accept("+")) c.expect("SYNTHESIZED");
        c.expect(")");
        net.connections.push_back(conn);
    }
    while (!c.empty()) {
        const auto token = c.take();
        if (token.text != "+") continue;
        const auto key = c.take().text;
        if (key == "SUBNET" || key == "VPIN")
            throw std::runtime_error("Unsupported DEF net property: " + key);
        if (key == "USE") net.use = c.take().text;
        if (key == "ROUTED" || key == "FIXED" || key == "COVER"
            || key == "SHIELD" || key == "NOSHIELD") {
            while (c.peek().text != "+" && c.peek().text != ";") c.take();
            net.oldRoutes.push_back({token.begin, c.peek().begin});
        }
    }
    return net;
}

void blockage(const std::vector<Token>& tokens, Design& design) {
    Cursor c(tokens);
    c.expect("-");
    if (!c.accept("LAYER")) return; // PLACEMENT blockage 不等于金属障碍物。
    const auto layer = c.take().text;
    while (!c.empty()) {
        const auto key = c.take().text;
        if (key == "RECT") design.blockages.push_back({layer, rectangle(c)});
        if (key == "POLYGON") throw std::runtime_error("POLYGON blockage is not supported");
    }
}

void skipSection(Cursor& c, const std::string& name) {
    while (!c.empty()) {
        if (c.take().text == "END" && c.accept(name)) return;
    }
    throw std::runtime_error("Missing END " + name);
}

} // namespace

Design parseDefText(const std::string& text) {
    const auto tokens = tokenize(text);
    Cursor c(tokens);
    Design design;
    design.source = text;
    bool ended = false;
    while (!c.empty()) {
        const auto key = c.take().text;
        if (key == "DESIGN") {
            design.name = c.take().text;
            c.expect(";");
        } else if (key == "UNITS") {
            c.expect("DISTANCE"); c.expect("MICRONS");
            design.dbuPerMicron = integer(c.take().text);
            c.expect(";");
        } else if (key == "DIEAREA") {
            design.die = rectangle(c);
            c.expect(";"); // 多边形 DIEAREA 明确报错。
        } else if (key == "GCELLGRID") {
            GridPattern pattern;
            const auto axis = c.take().text;
            if (axis != "X" && axis != "Y") throw std::runtime_error("Invalid GCELLGRID axis");
            pattern.axis = axis[0];
            pattern.start = integer(c.take().text);
            c.expect("DO");
            const auto count = integer(c.take().text);
            if (count < 1 || count > 1000000) throw std::runtime_error("Invalid GCELLGRID count");
            pattern.count = static_cast<int>(count);
            c.expect("STEP");
            pattern.step = integer(c.take().text);
            if (pattern.step <= 0) throw std::runtime_error("Invalid GCELLGRID step");
            c.expect(";");
            design.gridPatterns.push_back(pattern);
        } else if (key == "COMPONENTS") {
            section(c, key, [&](const auto& t) {
                const auto inst = component(t);
                if (!design.instances.emplace(inst.name, inst).second)
                    throw std::runtime_error("Duplicate instance: " + inst.name);
            });
        } else if (key == "PINS") {
            section(c, key, [&](const auto& t) {
                const auto pin = ioPin(t);
                if (!design.pins.emplace(pin.name, pin).second)
                    throw std::runtime_error("Duplicate DEF pin: " + pin.name);
            });
        } else if (key == "NETS") {
            if (!design.nets.empty()) throw std::runtime_error("Duplicate NETS section");
            section(c, key, [&](const auto& t) { design.nets.push_back(netRecord(t)); });
        } else if (key == "BLOCKAGES") {
            section(c, key, [&](const auto& t) { blockage(t, design); });
        } else if (key == "PROPERTYDEFINITIONS" || key == "VIAS" || key == "SPECIALNETS"
                   || key == "REGIONS" || key == "GROUPS" || key == "NONDEFAULTRULES"
                   || key == "SCANCHAINS" || key == "SLOTS" || key == "FILLS"
                   || key == "PINPROPERTIES" || key == "CONSTRAINTS" || key == "ASSERTIONS") {
            // 不参与本教学路由，但原文会完整保留在输出 DEF 中。
            skipSection(c, key);
        } else if (key == "END") {
            c.expect("DESIGN");
            ended = true;
            if (!c.empty()) throw std::runtime_error("Tokens after END DESIGN");
        } else {
            c.statement(); // ROW、TRACKS、VERSION 等。
        }
    }
    if (!ended || design.name.empty() || design.dbuPerMicron <= 0
        || design.die.x1 >= design.die.x2 || design.die.y1 >= design.die.y2)
        throw std::runtime_error("DEF needs DESIGN, positive UNITS, rectangular DIEAREA, END DESIGN");
    return design;
}

Design parseDef(const std::string& path) { return parseDefText(readText(path)); }

} // namespace router
