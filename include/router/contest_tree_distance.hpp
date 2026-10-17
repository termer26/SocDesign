#pragma once
#include "router/gcell_grid.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>

namespace router::contest {
// Exact nearest-target distance in the obstacle-free XY/VIA metric. Unlike a
// single bounding rectangle, this also guides searches inside a sparse tree.
// New targets are scanned until a batch rebuild; no target is ever omitted.
class TreeDistance {
public:
    explicit TreeDistance(const GCellGrid& grid) : grid_(grid) {}
    void clear() { points_.clear(); nodes_.clear(); indexed_ = 0; root_ = -1; }
    void add(Cell cell) { points_.push_back({grid_.center(cell), cell.layer}); }
    void prepare() {
        if (points_.empty() || (root_ >= 0 && points_.size() - indexed_ < 128)) return;
        order_.resize(points_.size());
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        nodes_.clear();
        nodes_.reserve(points_.size());
        root_ = build(0, order_.size());
        indexed_ = points_.size();
    }
    double distance(Cell cell) const {
        const auto p = grid_.center(cell);
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t i = indexed_; i < points_.size(); ++i)
            best = std::min(best, toPoint(p, cell.layer, points_[i]));
        query(root_, p, cell.layer, best);
        return best;
    }
private:
    struct Target { Point point; int layer; };
    struct Node { std::size_t point; Rect box; int left = -1, right = -1; };
    int build(std::size_t first, std::size_t last) {
        if (first == last) return -1;
        const auto p = points_[order_[first]].point;
        Rect box{p.x, p.y, p.x, p.y};
        for (auto i = first + 1; i < last; ++i) {
            const auto q = points_[order_[i]].point;
            box.x1 = std::min(box.x1, q.x); box.x2 = std::max(box.x2, q.x);
            box.y1 = std::min(box.y1, q.y); box.y2 = std::max(box.y2, q.y);
        }
        const bool splitX = box.x2 - box.x1 >= box.y2 - box.y1;
        const auto middle = first + (last - first) / 2;
        std::nth_element(order_.begin() + first, order_.begin() + middle, order_.begin() + last,
            [&](std::size_t a, std::size_t b) {
                const auto& x = points_[a]; const auto& y = points_[b];
                if (splitX) return std::tie(x.point.x, x.point.y, x.layer) < std::tie(y.point.x, y.point.y, y.layer);
                return std::tie(x.point.y, x.point.x, x.layer) < std::tie(y.point.y, y.point.x, y.layer);
            });
        const int index = static_cast<int>(nodes_.size());
        nodes_.push_back({order_[middle], box});
        const int left = build(first, middle), right = build(middle + 1, last);
        nodes_[index].left = left; nodes_[index].right = right;
        return index;
    }
    double toPoint(Point p, int layer, const Target& target) const {
        return static_cast<double>(std::abs(p.x - target.point.x) + std::abs(p.y - target.point.y)) / grid_.dbu()
            + grid_.layerLowerBound(layer, target.layer);
    }
    double bound(int index, Point p) const {
        if (index < 0) return std::numeric_limits<double>::infinity();
        const auto& box = nodes_[index].box;
        return static_cast<double>(std::max({box.x1 - p.x, Coord{0}, p.x - box.x2})
            + std::max({box.y1 - p.y, Coord{0}, p.y - box.y2})) / grid_.dbu();
    }
    void query(int index, Point p, int layer, double& best) const {
        if (index < 0 || bound(index, p) >= best) return;
        const auto& node = nodes_[index];
        best = std::min(best, toPoint(p, layer, points_[node.point]));
        int first = node.left, second = node.right;
        if (bound(first, p) > bound(second, p)) std::swap(first, second);
        query(first, p, layer, best);
        query(second, p, layer, best);
    }
    const GCellGrid& grid_;
    std::vector<Target> points_;
    std::vector<std::size_t> order_;
    std::vector<Node> nodes_;
    std::size_t indexed_ = 0;
    int root_ = -1;
};
} // namespace router::contest
