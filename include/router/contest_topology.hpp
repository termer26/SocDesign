#pragma once
#include "router/gcell_grid.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace router::contest {
// A die is a connected component of the ordinary-VIA layer graph. Removing
// TSV links keeps classification independent of instance and net names.
class DieTopology {
public:
    DieTopology(const GCellGrid& grid, const std::set<std::string>& crossNames = {})
        : die_(grid.layerCount(), -1) {
        for (int z = 0; z < grid.layerCount(); ++z) for (const auto& via : grid.viaLinks(z)) {
            auto name = via.name;
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool cross = crossNames.empty()
                ? name.find("tsv") != std::string::npos || name.find("hb") != std::string::npos
                  || name.find("hybrid") != std::string::npos
                : crossNames.count(via.name) != 0;
            if (cross) cross_.insert(std::minmax(z, via.layer));
        }
        int component = 0;
        for (int z = 0; z < grid.layerCount(); ++z) if (die_[z] < 0) {
            std::vector<int> pending{z}; die_[z] = component;
            for (std::size_t i = 0; i < pending.size(); ++i)
                for (const auto& via : grid.viaLinks(pending[i]))
                    if (!cross_.count(std::minmax(pending[i], via.layer)) && die_[via.layer] < 0) {
                        die_[via.layer] = component; pending.push_back(via.layer);
                    }
            ++component;
        }
    }
    int die(int layer) const { return die_.at(layer); }
    std::vector<bool> mask(int component) const {
        std::vector<bool> result(die_.size());
        for (std::size_t i = 0; i < die_.size(); ++i) result[i] = die_[i] == component;
        return result;
    }
    const std::set<std::pair<int,int>>& crossings() const { return cross_; }
private:
    std::vector<int> die_;
    std::set<std::pair<int,int>> cross_;
};
}
