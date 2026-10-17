#include "router/contest_resources.hpp"
#include "router/def_parser.hpp"
#include "router/lef_parser.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

// Uses the production C++ loader. The optional reference is the scorer's
// headerless, row-major heat_gcell.csv, not a second implementation of fill.
int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) {
            std::cerr << "Usage: contest_thermal_check input.lef input.def thermal.json [heat_gcell.csv]\n";
            return 1;
        }
        const auto started = std::chrono::steady_clock::now();
        const auto library = router::parseLef(argv[1]);
        const auto design = router::parseDef(argv[2]);
        router::GridOptions options;
        options.maxNodes = 50000000;
        router::GCellGrid grid(design, library, options);
        router::contest::Resources resources(design, grid);
        resources.loadThermal(argv[3]);
        const auto& thermal = resources.thermal();
        std::size_t compared = 0, mismatches = 0;
        double maxError = 0;
        if (argc == 5) {
            std::ifstream input(argv[4]);
            if (!input) throw std::runtime_error("Cannot open reference heat_gcell.csv");
            std::string line;
            int rows = 0;
            while (std::getline(input,line)) {
                std::istringstream row(line);
                std::string field;
                int columns = 0;
                while (std::getline(row,field,',')) {
                    if (compared >= thermal.temperature.size())
                        throw std::runtime_error("Reference CSV has too many cells");
                    std::istringstream number(field);
                    double expected;
                    if (!(number >> expected) || !std::isfinite(expected))
                        throw std::runtime_error("Invalid temperature in reference CSV");
                    number >> std::ws;
                    if (!number.eof()) throw std::runtime_error("Unexpected reference CSV text");
                    const double error = std::abs(thermal.temperature[compared] - expected);
                    maxError = std::max(maxError,error);
                    if (error > 1e-7) ++mismatches;
                    ++compared; ++columns;
                }
                if (columns != grid.width()) throw std::runtime_error("Reference column count differs from grid");
                ++rows;
            }
            if (input.bad() || rows != grid.height())
                throw std::runtime_error("Reference row count differs from grid or read failed");
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        std::cout << std::setprecision(15)
            << "{\n  \"scope\": \"input and thermal loading; optional scorer CSV comparison; no routing\",\n"
            << "  \"grid_nodes\": " << grid.size() << ", \"nets\": " << design.nets.size() << ",\n"
            << "  \"columns\": " << grid.width() << ", \"rows\": " << grid.height() << ",\n"
            << "  \"samples\": " << thermal.mappedNodes << ", \"sampled_cells\": " << thermal.sampledCells
            << ", \"filled_cells\": " << thermal.filledCells << ", \"clipped_samples\": " << thermal.clippedNodes << ",\n"
            << "  \"coordinates\": \"" << thermal.coordinates << "\", \"offset_x_dbu\": " << thermal.offsetX
            << ", \"offset_y_dbu\": " << thermal.offsetY << ",\n"
            << "  \"compared_cells\": " << compared << ", \"mismatches\": " << mismatches
            << ", \"max_absolute_error\": " << maxError << ",\n"
            << "  \"elapsed_seconds\": " << seconds << "\n}\n";
        return mismatches ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
