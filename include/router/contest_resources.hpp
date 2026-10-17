#pragma once
#include "router/gcell_grid.hpp"
#include <set>

namespace router::contest {
struct ResourceOptions {
    double presentWeight = 2.0, historyWeight = 1.0, thermalWeight = 12.0;
    double crossViaCost = 8.0;
    double capacityScale = 1.0;
    int fallbackCapacity = 8;
    std::set<std::string> crossVias;
    std::string thermalCoordinates = "auto"; // auto, absolute, die-local
    double thermalBoundaryToleranceDbu = 100;
};
struct ThermalData {
    bool loaded = false, synthetic = false;
    double ambient = 0;
    std::vector<double> temperature, normalized;
    std::size_t mappedNodes = 0;
    std::size_t sampledCells = 0, filledCells = 0, clippedNodes = 0;
    std::string coordinates;
    Coord offsetX = 0, offsetY = 0;
};
class Resources {
public:
    Resources(const Design& design, const GCellGrid& grid, ResourceOptions options = {});
    void loadThermal(const std::string& path);
    double stepCost(Cell from, Cell to) const;
    bool allowed(Cell from, Cell to) const;
    bool isCross(int fromLayer, int toLayer) const;
    void commit(const RoutedNet& route, int delta = 1);
    void clearUsage();
    void updateHistory();
    void setPresentFactor(double factor);
    std::vector<int> routeNodes(const RoutedNet& route) const;
    std::vector<std::pair<int,int>> routeEdges(const RoutedNet& route) const;
    long long overflow() const;
    int maxOverflow() const;
    std::size_t overflowCells() const;
    std::size_t usedCells() const;
    double correlation(bool aboveAmbient = false) const;
    double staticCost(const RoutedNet& route) const;
    const std::vector<int>& capacities() const { return capacity_; }
    const std::vector<int>& usage() const { return usage_; }
    const std::vector<int>& tsvCounts() const { return tsv_; }
    const ThermalData& thermal() const { return thermal_; }
    const ResourceOptions& options() const { return options_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    const std::set<std::string>& crossViaNames() const { return crossNames_; }
private:
    const GCellGrid& grid_;
    ResourceOptions options_;
    ThermalData thermal_;
    std::vector<int> capacity_, usage_, tsv_;
    std::vector<double> history_;
    std::set<std::pair<int,int>> crossPairs_;
    std::set<std::string> crossNames_;
    std::vector<std::string> warnings_;
    double presentFactor_ = 1.0;
};
}
