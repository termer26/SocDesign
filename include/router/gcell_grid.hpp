#pragma once
#include "router/types.hpp"
#include <unordered_map>
#include <unordered_set>

namespace router {

struct GridOptions {
    Coord fallbackStep = 0; // 没有 GCELLGRID 时需显式指定，单位 DBU。
    double viaCost = 5.0;   // 单位为等效微米线长。
    double congestionWeight = 2.0;
    int capacity = 8;       // 教学模型中每条格边的估计容量。
    std::int64_t maxNodes = 2000000;
    std::int64_t denseMemoryMiB = 4096; // Budget for dense routing arrays, not a peak-RSS guarantee.
};
struct ViaLink { int layer = 0; std::string name; };

class GCellGrid {
public:
    GCellGrid(const Design& design, const Library& library, GridOptions options = {});
    int width() const { return static_cast<int>(xs_.size()) - 1; }
    int height() const { return static_cast<int>(ys_.size()) - 1; }
    int layerCount() const { return static_cast<int>(layers_.size()); }
    int size() const { return width() * height() * layerCount(); }
    bool contains(Point p) const;
    bool valid(Cell c) const;
    Cell locate(Point p, int layer) const;
    Point center(Cell c) const;
    int id(Cell c) const;
    Cell cell(int id) const;
    int layerIndex(const std::string& name) const;
    const Layer& layer(int index) const { return layers_.at(index); }
    const std::vector<Coord>& xLines() const { return xs_; }
    const std::vector<Coord>& yLines() const { return ys_; }
    const std::vector<ViaLink>& viaLinks(int layer) const { return vias_.at(layer); }
    std::string viaName(int from, int to) const;

    void addObstacles(const std::vector<PinShape>& shapes);
    void blockCell(Cell c); // 单元测试与自定义网格也可使用。
    bool blocked(Cell c) const;
    bool pointBlocked(Point p, int layer) const;
    bool pointBlockedByOther(Point p, int layer, const std::string& pinOwner) const;
    bool segmentBlocked(Point from, Point to, int layer) const;
    // Diagnostic access to the stored obstacle rectangles of a routing layer.
    const std::vector<Rect>& obstacleRects(int layer) const { return obstacleRects_.at(layer); }
    std::vector<Cell> neighbors(Cell c) const;
    double stepCost(Cell from, Cell to) const;
    double layerLowerBound(int from, int to) const;
    void commit(const std::vector<std::vector<Cell>>& branches);
    std::size_t overflowEdges() const;
    const GridOptions& options() const { return options_; }
    Coord dbu() const { return dbu_; }
private:
    static std::uint64_t edgeKey(int a, int b);
    std::vector<Coord> xs_, ys_;
    std::vector<Layer> layers_;
    std::vector<std::vector<ViaLink>> vias_;
    std::vector<std::vector<double>> layerDistance_;
    std::vector<bool> blocked_;
    std::vector<std::vector<Rect>> obstacleRects_;
    std::vector<std::vector<PinShape>> obstacleShapes_;
    std::unordered_set<std::uint64_t> blockedEdges_;
    std::unordered_map<std::uint64_t, int> usage_;
    GridOptions options_;
    Coord dbu_ = 0;
};

} // namespace router
