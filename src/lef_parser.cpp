#include "router/lef_parser.hpp"
#include "router/tokenizer.hpp"
#include <algorithm>

namespace router {
namespace {

// PORT / OBS 以无名称的 END 结束。只支持矩形，不把多边形误当矩形。
std::vector<LefRect> geometry(Cursor& c, std::size_t& normalizedRectangles) {
    std::vector<LefRect> shapes;
    std::string layer;
    while (!c.accept("END")) {
        const auto key = c.take().text;
        if (key == "LAYER") {
            const auto s = c.statement();
            layer = s.at(0).text;
        } else if (key == "RECT") {
            if (layer.empty()) throw std::runtime_error("LEF RECT without LAYER");
            const auto s = c.statement();
            if (s.size() != 5)
                throw std::runtime_error("LEF RECT MASK/ITERATE is not supported");
            LefRect r{layer, real(s[0].text), real(s[1].text), real(s[2].text), real(s[3].text)};
            if (r.x1 == r.x2 || r.y1 == r.y2) throw std::runtime_error("Zero-area LEF rectangle");
            // Public case005 contains reversed Y corner pairs. Preserve both
            // coordinates and interpret their bounding rectangle in memory.
            if (r.x1 > r.x2 || r.y1 > r.y2) {
                ++normalizedRectangles;
                if (r.x1 > r.x2) std::swap(r.x1,r.x2);
                if (r.y1 > r.y2) std::swap(r.y1,r.y2);
            }
            shapes.push_back(r);
        } else if (key == "POLYGON" || key == "PATH" || key == "VIA") {
            throw std::runtime_error("Unsupported LEF PORT/OBS geometry: " + key);
        } else {
            c.statement(); // CLASS、SPACING 等非几何属性。
        }
    }
    return shapes;
}

LefPin parsePin(Cursor& c, std::size_t& normalizedRectangles) {
    LefPin pin;
    pin.name = c.take().text;
    while (!c.accept("END")) {
        const auto key = c.take().text;
        if (key == "PORT") {
            auto shapes = geometry(c,normalizedRectangles);
            pin.shapes.insert(pin.shapes.end(), shapes.begin(), shapes.end());
        } else {
            const auto s = c.statement();
            if (key == "USE") pin.use = s.at(0).text;
        }
    }
    c.expect(pin.name);
    return pin;
}

Macro parseMacro(Cursor& c, std::size_t& normalizedRectangles) {
    Macro macro;
    macro.name = c.take().text;
    while (!c.accept("END")) {
        const auto key = c.take().text;
        if (key == "PIN") {
            auto pin = parsePin(c,normalizedRectangles);
            if (!macro.pins.emplace(pin.name, pin).second)
                throw std::runtime_error("Duplicate LEF pin: " + macro.name + "/" + pin.name);
        } else if (key == "OBS") {
            auto shapes = geometry(c,normalizedRectangles);
            macro.obstructions.insert(macro.obstructions.end(), shapes.begin(), shapes.end());
        } else {
            const auto s = c.statement();
            if (key == "CLASS") {
                macro.cellClass = s.at(0).text;
            } else if (key == "ORIGIN") {
                macro.originX = real(s.at(0).text);
                macro.originY = real(s.at(1).text);
            } else if (key == "SIZE") {
                macro.width = real(s.at(0).text);
                if (s.at(1).text != "BY") throw std::runtime_error("Expected SIZE x BY y");
                macro.height = real(s.at(2).text);
            }
        }
    }
    c.expect(macro.name);
    if (macro.width <= 0 || macro.height <= 0)
        throw std::runtime_error("Missing/invalid LEF SIZE: " + macro.name);
    return macro;
}

Layer parseLayer(Cursor& c) {
    Layer layer;
    layer.name = c.take().text;
    while (!c.accept("END")) {
        const auto key = c.take().text;
        const auto s = c.statement();
        if (key == "TYPE") layer.type = s.at(0).text;
        if (key == "DIRECTION") layer.direction = s.at(0).text;
        if (key == "WIDTH") layer.width = real(s.at(0).text);
        if (key == "PITCH") layer.pitch = real(s.at(0).text);
    }
    c.expect(layer.name);
    return layer;
}

Via parseVia(Cursor& c) {
    Via via;
    via.name = c.take().text;
    via.isDefault = c.accept("DEFAULT");
    while (!c.accept("END")) {
        const auto key = c.take().text;
        const auto s = c.statement();
        if (key == "LAYER") via.layers.push_back(s.at(0).text);
        if (key == "VIARULE")
            throw std::runtime_error("Generated LEF VIA is not supported: " + via.name);
    }
    c.expect(via.name);
    return via;
}

void skipBlock(Cursor& c, const std::string& endName) {
    while (!c.empty()) {
        if (c.take().text == "END" && c.accept(endName)) return;
    }
    throw std::runtime_error("Missing END " + endName);
}

} // namespace

Library parseLefText(const std::string& text) {
    const auto tokens = tokenize(text);
    Cursor c(tokens);
    Library library;
    while (!c.empty()) {
        const auto key = c.take().text;
        if (key == "MACRO") {
            auto macro = parseMacro(c,library.normalizedRectangles);
            if (!library.macros.emplace(macro.name, macro).second)
                throw std::runtime_error("Duplicate LEF MACRO: " + macro.name);
        } else if (key == "LAYER") {
            auto layer = parseLayer(c);
            if (std::any_of(library.layers.begin(), library.layers.end(),
                            [&](const Layer& v) { return v.name == layer.name; }))
                throw std::runtime_error("Duplicate LEF LAYER: " + layer.name);
            library.layers.push_back(layer);
        } else if (key == "VIA") {
            library.vias.push_back(parseVia(c));
        } else if (key == "VIARULE" || key == "SITE") {
            const auto name = c.take().text;
            skipBlock(c, name);
        } else if (key == "UNITS" || key == "PROPERTYDEFINITIONS" || key == "SPACING") {
            skipBlock(c, key);
        } else if (key == "END") {
            c.expect("LIBRARY");
            if (!c.empty()) throw std::runtime_error("Tokens after END LIBRARY");
        } else {
            c.statement();
        }
    }
    return library;
}

Library parseLef(const std::string& path) { return parseLefText(readText(path)); }

} // namespace router
