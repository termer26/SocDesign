#pragma once
#include "router/contest_resources.hpp"
#include <functional>

namespace router::contest {
struct Options {
    int iterations = 8;
    double timeLimitSeconds = 300;
    double astarWeight = 1.0; // 1: exact A*; >1: weighted search, faster but not necessarily shortest.
    int thermalSearchRadius = 64; // Extra TSV candidates, in GCells; 0 keeps the local search.
    int tsvRouteTrials = 1; // Successful candidate trees to compare before choosing a TSV.
    bool inputOrder = false;
    std::function<void(std::size_t,std::size_t,double)> netProgress;
};
struct Iteration {
    int index = 0;
    std::size_t failed = 0, expanded = 0, rerouted = 0, crossVias = 0;
    long long overflow = 0;
    double cost = 0, correlation = 0, seconds = 0;
    bool selected = false;
};
struct Result {
    std::vector<RoutedNet> routes;
    std::vector<Iteration> iterations;
    std::size_t expanded = 0;
    bool timeLimitReached = false;
};
Result route(const Design& design, const PinDatabase& pins, const GCellGrid& grid,
             Resources& resources, Options options = {},
             const std::function<void(const Iteration&)>& progress = {});
}
