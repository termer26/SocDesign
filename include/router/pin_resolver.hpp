#pragma once
#include "router/types.hpp"
namespace router {
// rotatePoint 仅作绕原点旋转/镜像；placeMacroPoint 再补齐单元外框与 DEF 放置点。
Point rotatePoint(Point p, const std::string& orientation);
Point placeMacroPoint(Point p, Coord width, Coord height,
                      Point position, const std::string& orientation);
PinDatabase resolvePins(const Design& design, const Library& library);
const PinLocation& lookupPin(const PinDatabase& pins, const Connection& connection);
void writePinsCsv(const std::string& path, const PinDatabase& pins);
} // namespace router
