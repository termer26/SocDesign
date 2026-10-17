#include "router/contest_resources.hpp"
#include "router/tokenizer.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace router::contest {
namespace {

// A small strict JSON reader keeps the command-line solver dependency-free.
// Duplicate object keys, non-finite numbers, bad escapes and trailing text fail.
struct Json {
    enum class Kind { Null, Boolean, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;
    const Json* get(const std::string& name) const {
        const auto it = object.find(name);
        return it == object.end() ? nullptr : &it->second;
    }
};

class JsonReader {
public:
    JsonReader(const std::string& source, std::vector<std::array<double, 4>>& nodes, std::size_t limit)
        : source_(source), nodes_(nodes), nodeLimit_(limit) {}
    Json read() {
        if (source_.compare(0, 3, "\xEF\xBB\xBF") == 0) at_ = 3;
        auto result = value(0);
        whitespace();
        if (at_ != source_.size()) fail("trailing text");
        return result;
    }
private:
    const std::string& source_;
    std::vector<std::array<double, 4>>& nodes_;
    std::size_t nodeLimit_;
    std::size_t at_ = 0;
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error("Invalid thermal JSON at byte " + std::to_string(at_) + ": " + reason);
    }
    void whitespace() {
        while (at_ < source_.size() && (source_[at_] == ' ' || source_[at_] == '\t'
               || source_[at_] == '\r' || source_[at_] == '\n')) ++at_;
    }
    bool accept(char c) {
        whitespace();
        if (at_ < source_.size() && source_[at_] == c) { ++at_; return true; }
        return false;
    }
    void expect(char c) { if (!accept(c)) fail(std::string("expected '") + c + "'"); }
    unsigned hex4() {
        unsigned result = 0;
        for (int n = 0; n < 4; ++n) {
            if (at_ == source_.size()) fail("incomplete Unicode escape");
            const auto c = source_[at_++];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid Unicode escape");
            result = result * 16 + digit;
        }
        return result;
    }
    void utf8(std::string& out, unsigned code) {
        if (code <= 0x7f) out.push_back(static_cast<char>(code));
        else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 63)));
        } else if (code <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (code & 63)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 63)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (code & 63)));
        }
    }
    std::string string() {
        expect('"');
        std::string out;
        while (at_ < source_.size()) {
            const unsigned char c = static_cast<unsigned char>(source_[at_++]);
            if (c == '"') return out;
            if (c < 0x20) fail("unescaped control character");
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (at_ == source_.size()) fail("incomplete escape");
            switch (source_[at_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (at_ + 2 > source_.size() || source_[at_] != '\\' || source_[at_ + 1] != 'u')
                        fail("missing low Unicode surrogate");
                    at_ += 2;
                    const auto low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail("invalid low Unicode surrogate");
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) fail("unpaired Unicode surrogate");
                utf8(out, code);
                break;
            }
            default: fail("unknown string escape");
            }
        }
        fail("unterminated string");
    }
    Json nodeArray() {
        // Read the large numeric payload directly instead of retaining a JSON
        // object for every scalar. Metadata still uses the small generic tree.
        expect('[');
        Json result;
        result.kind = Json::Kind::Array;
        if (accept(']')) return result;
        do {
            if (nodes_.size() >= nodeLimit_) fail("thermal node count exceeds the 10000000-sample limit");
            expect('[');
            std::array<double, 4> node{};
            for (std::size_t i = 0; i < node.size(); ++i) {
                if (i) expect(',');
                const auto coordinate = value(2);
                if (coordinate.kind != Json::Kind::Number) fail("thermal nodes must contain four finite numbers");
                node[i] = coordinate.number;
            }
            expect(']');
            nodes_.push_back(node);
            if (accept(']')) return result;
            expect(',');
        } while (true);
    }
    Json value(int depth, bool retain = true) {
        if (depth > 64) fail("nesting exceeds 64 levels");
        whitespace();
        if (at_ == source_.size()) fail("expected a value");
        Json result;
        const char c = source_[at_];
        if (c == '{') {
            ++at_;
            result.kind = Json::Kind::Object;
            std::set<std::string> keys;
            if (accept('}')) return result;
            do {
                const auto key = string();
                expect(':');
                if (!keys.insert(key).second) fail("duplicate key: " + key);
                const bool wanted = retain && (depth == 0 ? key == "nodes" || key == "meta" : depth == 1
                    ? key == "coordinateUnit" || key == "temperatureUnit" || key == "ambientTemp"
                      || key == "synthetic" || key == "dieNames" : true);
                auto item = depth == 0 && key == "nodes" ? nodeArray() : value(depth + 1, wanted);
                if (wanted) result.object.emplace(key, std::move(item));
                if (accept('}')) return result;
                expect(',');
            } while (true);
        }
        if (c == '[') {
            ++at_;
            result.kind = Json::Kind::Array;
            if (accept(']')) return result;
            do {
                auto item = value(depth + 1, retain);
                if (retain) result.array.push_back(std::move(item));
                if (accept(']')) return result;
                expect(',');
            } while (true);
        }
        if (c == '"') { result.kind = Json::Kind::String; result.string = string(); return result; }
        if (source_.compare(at_, 4, "null") == 0) { at_ += 4; return result; }
        if (source_.compare(at_, 4, "true") == 0) {
            at_ += 4; result.kind = Json::Kind::Boolean; result.boolean = true; return result;
        }
        if (source_.compare(at_, 5, "false") == 0) {
            at_ += 5; result.kind = Json::Kind::Boolean; return result;
        }
        const auto begin = at_;
        if (source_[at_] == '-') ++at_;
        if (at_ == source_.size()) fail("incomplete number");
        if (source_[at_] == '0') ++at_;
        else {
            if (source_[at_] < '1' || source_[at_] > '9') fail("expected a JSON value");
            while (at_ < source_.size() && source_[at_] >= '0' && source_[at_] <= '9') ++at_;
        }
        if (at_ < source_.size() && source_[at_] == '.') {
            ++at_;
            const auto digits = at_;
            while (at_ < source_.size() && source_[at_] >= '0' && source_[at_] <= '9') ++at_;
            if (digits == at_) fail("missing fractional digits");
        }
        if (at_ < source_.size() && (source_[at_] == 'e' || source_[at_] == 'E')) {
            ++at_;
            if (at_ < source_.size() && (source_[at_] == '-' || source_[at_] == '+')) ++at_;
            const auto digits = at_;
            while (at_ < source_.size() && source_[at_] >= '0' && source_[at_] <= '9') ++at_;
            if (digits == at_) fail("missing exponent digits");
        }
        const auto token = source_.substr(begin, at_ - begin);
        char* end = nullptr;
        result.number = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(result.number)) fail("non-finite number");
        result.kind = Json::Kind::Number;
        return result;
    }
};

void kind(const Json& value, Json::Kind expected, const std::string& field) {
    if (value.kind != expected) throw std::runtime_error("Thermal JSON has invalid type for " + field);
}
std::string lowercase(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::pair<int, int> pairKey(int a, int b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

struct Tracks {
    std::vector<std::set<Coord>> x, y;
    std::vector<bool> hasX, hasY;
};
Tracks readTracks(const Design& design, const GCellGrid& grid) {
    Tracks tracks;
    tracks.x.resize(grid.layerCount()); tracks.y.resize(grid.layerCount());
    tracks.hasX.resize(grid.layerCount()); tracks.hasY.resize(grid.layerCount());
    const auto tokens = tokenize(design.source);
    Cursor c(tokens);
    const std::set<std::string> sections{
        "COMPONENTS", "PINS", "NETS", "SPECIALNETS", "BLOCKAGES", "VIAS", "REGIONS",
        "GROUPS", "NONDEFAULTRULES", "SCANCHAINS", "SLOTS", "FILLS", "PINPROPERTIES",
        "CONSTRAINTS", "ASSERTIONS", "PROPERTYDEFINITIONS"
    };
    std::size_t expanded = 0;
    while (!c.empty()) {
        const auto key = c.take().text;
        if (key == "END") { c.expect("DESIGN"); break; }
        if (sections.count(key)) {
            bool ended = false;
            while (!c.empty())
                if (c.take().text == "END" && c.accept(key)) { ended = true; break; }
            if (!ended) throw std::runtime_error("Missing END " + key + " while reading TRACKS");
            continue;
        }
        if (key != "TRACKS") { c.statement(); continue; }
        const auto axis = c.take().text;
        if (axis != "X" && axis != "Y") throw std::runtime_error("Invalid TRACKS axis");
        const auto start = integer(c.take().text);
        c.expect("DO");
        const auto count = integer(c.take().text);
        c.expect("STEP");
        const auto step = integer(c.take().text);
        if (count < 1 || count > 5000000 || step <= 0)
            throw std::runtime_error("TRACKS requires positive STEP and DO in 1..5000000");
        if (c.accept("MASK")) {
            if (integer(c.take().text) < 0) throw std::runtime_error("Invalid TRACKS MASK");
            c.accept("SAMEMASK");
        }
        c.expect("LAYER");
        std::vector<int> layers;
        while (!c.accept(";")) {
            const auto name = c.take().text;
            const auto layer = grid.layerIndex(name);
            if (layer < 0) throw std::runtime_error("TRACKS references unknown routing layer: " + name);
            layers.push_back(layer);
        }
        if (layers.empty()) throw std::runtime_error("TRACKS has no LAYER names");
        std::sort(layers.begin(), layers.end());
        layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
        const auto low = axis == "X" ? grid.xLines().front() : grid.yLines().front();
        const auto high = axis == "X" ? grid.xLines().back() : grid.yLines().back();
        auto& coordinates = axis == "X" ? tracks.x : tracks.y;
        auto& present = axis == "X" ? tracks.hasX : tracks.hasY;
        for (const int layer : layers) present[layer] = true;
        // Resource counting merges coordinates, including partially overlapping patterns.
        for (Coord i = 0; i < count; ++i) {
            const long double point = static_cast<long double>(start) + static_cast<long double>(i) * step;
            if (point < std::numeric_limits<Coord>::min() || point > std::numeric_limits<Coord>::max())
                throw std::runtime_error("TRACKS coordinate overflow");
            if (point < low || point > high) continue;
            if (layers.size() > 10000000 - expanded)
                throw std::runtime_error("TRACKS expansion exceeds 10000000 entries");
            expanded += layers.size();
            for (const auto layer : layers) coordinates[layer].insert(static_cast<Coord>(point));
        }
    }
    return tracks;
}

std::vector<int> trackBins(const std::set<Coord>& tracks, const std::vector<Coord>& lines) {
    std::vector<int> result(lines.size() - 1, 0);
    for (const auto coordinate : tracks) {
        if (coordinate < lines.front() || coordinate > lines.back()) continue;
        auto bin = std::upper_bound(lines.begin(), lines.end(), coordinate) - lines.begin() - 1;
        bin = std::min(bin, static_cast<decltype(bin)>(result.size() - 1));
        ++result[static_cast<std::size_t>(bin)];
    }
    return result;
}

int scaledCapacity(int capacity, double scale) {
    if (capacity == 0) return 0;
    const auto scaled = std::floor(static_cast<long double>(capacity) * scale + 1.0e-9L);
    if (scaled > std::numeric_limits<int>::max()) throw std::runtime_error("Scaled capacity exceeds integer range");
    // Scaling a positive resource to one preserves reachability for congestion stress tests.
    return static_cast<int>(std::max(1.0L, scaled));
}

double baseCost(const GCellGrid& grid, const ResourceOptions& options, const ThermalData& thermal,
                bool cross, Cell from, Cell to) {
    double result = grid.stepCost(from, to);
    if (cross) {
        result += options.crossViaCost - grid.options().viaCost;
        if (thermal.loaded)
            result += options.thermalWeight * (1.0 - thermal.normalized[from.y * grid.width() + from.x]);
    }
    return result;
}

bool adjacent(const GCellGrid& grid, Cell from, Cell to) {
    if (!grid.valid(from) || !grid.valid(to)) return false;
    if (from.layer == to.layer)
        return std::abs(from.x - to.x) + std::abs(from.y - to.y) == 1;
    if (from.x != to.x || from.y != to.y) return false;
    for (const auto& link : grid.viaLinks(from.layer)) if (link.layer == to.layer) return true;
    return false;
}
} // namespace

Resources::Resources(const Design& design, const GCellGrid& grid, ResourceOptions options)
    : grid_(grid), options_(std::move(options)) {
    for (const auto weight : {options_.presentWeight, options_.historyWeight, options_.thermalWeight})
        if (!std::isfinite(weight) || weight < 0) throw std::runtime_error("Resource weights must be finite and nonnegative");
    if (!std::isfinite(options_.crossViaCost) || options_.crossViaCost < grid.options().viaCost)
        throw std::runtime_error("Cross-die VIA cost must be finite and at least the base VIA cost");
    if (!std::isfinite(options_.capacityScale) || options_.capacityScale <= 0 || options_.fallbackCapacity <= 0)
        throw std::runtime_error("Capacity scale and fallback capacity must be positive");
    capacity_.resize(grid.size()); usage_.assign(grid.size(), 0); history_.assign(grid.size(), 0);
    tsv_.assign(static_cast<std::size_t>(grid.width()) * grid.height(), 0);
    const auto tracks = readTracks(design, grid);
    for (int layer = 0; layer < grid.layerCount(); ++layer) {
        const auto& metal = grid.layer(layer);
        const bool horizontal = metal.direction == "HORIZONTAL", vertical = metal.direction == "VERTICAL";
        const bool hasTracks = (horizontal && tracks.hasY[layer]) || (vertical && tracks.hasX[layer]);
        const auto bins = hasTracks ? trackBins(horizontal ? tracks.y[layer] : tracks.x[layer],
                                                horizontal ? grid.yLines() : grid.xLines()) : std::vector<int>{};
        const bool usePitch = std::isfinite(metal.pitch) && metal.pitch > 0;
        if (!hasTracks) warnings_.push_back("Layer " + metal.name + " has no preferred-direction TRACKS; using "
                                            + (usePitch ? "LEF PITCH capacity estimate" : "fallback capacity"));
        if (!horizontal && !vertical)
            warnings_.push_back("Layer " + metal.name + " has no recognized preferred direction; using the smaller GCell span");
        for (int y = 0; y < grid.height(); ++y) for (int x = 0; x < grid.width(); ++x) {
            int capacity = options_.fallbackCapacity;
            if (hasTracks) capacity = bins[horizontal ? y : x];
            else if (usePitch) {
                const auto dx = grid.xLines()[x + 1] - grid.xLines()[x];
                const auto dy = grid.yLines()[y + 1] - grid.yLines()[y];
                const auto span = horizontal ? dy : vertical ? dx : std::min(dx, dy);
                // Decimal pitches such as 0.2 must not turn an exact five-track
                // estimate into four through the binary double representation.
                const long double estimated = std::floor(static_cast<long double>(span) / grid.dbu() / metal.pitch + 1.0e-9L);
                if (estimated > std::numeric_limits<int>::max())
                    throw std::runtime_error("LEF PITCH capacity exceeds integer range on " + metal.name);
                capacity = static_cast<int>(std::max(1.0L, estimated));
            }
            capacity_[grid.id({x, y, layer})] = scaledCapacity(capacity, options_.capacityScale);
        }
    }
    std::set<std::string> matched;
    for (int layer = 0; layer < grid.layerCount(); ++layer) for (const auto& link : grid.viaLinks(layer)) {
        if (layer >= link.layer) continue;
        const auto name = lowercase(link.name);
        const bool cross = options_.crossVias.empty()
            ? name.find("tsv") != std::string::npos || name.find("hb") != std::string::npos || name.find("hybrid") != std::string::npos
            : options_.crossVias.count(link.name) != 0;
        if (!cross) continue;
        crossPairs_.insert(pairKey(layer, link.layer));
        crossNames_.insert(link.name); matched.insert(link.name);
    }
    for (const auto& requested : options_.crossVias)
        if (!matched.count(requested))
            throw std::runtime_error("Cross-die VIA is not a selected grid VIA: " + requested);
    if (options_.crossVias.empty()) {
        std::string names;
        for (const auto& name : crossNames_) { if (!names.empty()) names += ", "; names += name; }
        warnings_.push_back(names.empty() ? "No cross-die VIA recognized from selected LEF VIA names"
                                         : "Cross-die VIA inferred from selected LEF VIA names: " + names);
    }
}

void Resources::loadThermal(const std::string& path) {
    const auto source = readText(path);
    std::vector<std::array<double, 4>> nodeRecords;
    const auto root = JsonReader(source, nodeRecords, 10000000).read();
    kind(root, Json::Kind::Object, "root");
    ThermalData next;
    std::vector<std::string> warnings;
    std::string coordinateUnit = "micron";
    const Json* meta = root.get("meta");
    if (meta) kind(*meta, Json::Kind::Object, "meta");
    const auto field = [&](const std::string& name) { return meta ? meta->get(name) : nullptr; };
    if (const auto unit = field("coordinateUnit")) {
        kind(*unit, Json::Kind::String, "meta.coordinateUnit");
        coordinateUnit = lowercase(unit->string);
        if (coordinateUnit != "micron" && coordinateUnit != "dbu")
            throw std::runtime_error("Thermal coordinateUnit must be micron or dbu");
    } else warnings.push_back("Thermal coordinateUnit is absent; interpreting XY coordinates as microns");
    if (const auto unit = field("temperatureUnit")) {
        kind(*unit, Json::Kind::String, "meta.temperatureUnit");
        const auto name = lowercase(unit->string);
        if (name != "degc" && name != "c" && name != "celsius")
            throw std::runtime_error("Thermal temperatureUnit must be degC");
    }
    if (const auto ambient = field("ambientTemp")) {
        kind(*ambient, Json::Kind::Number, "meta.ambientTemp"); next.ambient = ambient->number;
    } else warnings.push_back("Thermal ambientTemp is absent; above-ambient correlation uses 0 degC");
    if (const auto synthetic = field("synthetic")) {
        kind(*synthetic, Json::Kind::Boolean, "meta.synthetic"); next.synthetic = synthetic->boolean;
        if (next.synthetic) warnings.push_back("Thermal map is marked synthetic; thermal metrics are not physical signoff results");
    }
    const auto nodes = root.get("nodes");
    if (!nodes) throw std::runtime_error("Thermal JSON requires a nodes array");
    kind(*nodes, Json::Kind::Array, "nodes");
    if (nodeRecords.empty()) throw std::runtime_error("Thermal nodes array is empty");
    const std::size_t xySize = static_cast<std::size_t>(grid_.width()) * grid_.height();
    const long double scale = coordinateUnit == "micron" ? grid_.dbu() : 1;
    if (options_.thermalCoordinates != "auto" && options_.thermalCoordinates != "absolute"
        && options_.thermalCoordinates != "die-local") throw std::runtime_error("Invalid thermal coordinate mode");
    if (!std::isfinite(options_.thermalBoundaryToleranceDbu) || options_.thermalBoundaryToleranceDbu < 0)
        throw std::runtime_error("Invalid thermal boundary tolerance");
    long double minX = std::numeric_limits<long double>::infinity(), minY = minX, maxX = -minX, maxY = -minX;
    for (const auto& node : nodeRecords) {
        minX = std::min(minX, node[0]*scale); maxX = std::max(maxX, node[0]*scale);
        minY = std::min(minY, node[1]*scale); maxY = std::max(maxY, node[1]*scale);
    }
    bool local = options_.thermalCoordinates == "die-local";
    if (options_.thermalCoordinates == "auto" && (grid_.xLines().front() || grid_.yLines().front()))
        local = std::abs(minX) <= 1 && std::abs(minY) <= 1
             && std::abs(maxX - (grid_.xLines().back()-grid_.xLines().front())) <= 1
             && std::abs(maxY - (grid_.yLines().back()-grid_.yLines().front())) <= 1;
    next.coordinates = local ? "die-local" : "absolute";
    next.offsetX = local ? grid_.xLines().front() : 0;
    next.offsetY = local ? grid_.yLines().front() : 0;
    if (local && options_.thermalCoordinates == "auto")
        warnings.push_back("Thermal XY spans match DIEAREA dimensions; applying die-local origin offset");
    std::set<double> dies;
    std::vector<long double> sums(xySize, 0);
    std::vector<std::size_t> counts(xySize, 0);
    const auto snapBoundary = [](long double value, Coord low, Coord high) {
        // JSON numbers arrive as doubles. Decimal micron coordinates exactly
        // on the boundary can acquire a tiny conversion error in DBU.
        for (const Coord boundary : {low,high}) {
            const auto epsilon = 8 * std::numeric_limits<double>::epsilon()
                * std::max({1.0L,std::abs(value),std::abs(static_cast<long double>(boundary))});
            if (std::abs(value-boundary) <= epsilon) return static_cast<long double>(boundary);
        }
        return value;
    };
    for (std::size_t i = 0; i < nodeRecords.size(); ++i) {
        const auto& node = nodeRecords[i];
        const auto label = "nodes[" + std::to_string(i) + "]";
        const long double x = snapBoundary(static_cast<long double>(node[0]) * scale + next.offsetX,
                                          grid_.xLines().front(),grid_.xLines().back());
        const long double y = snapBoundary(static_cast<long double>(node[1]) * scale + next.offsetY,
                                          grid_.yLines().front(),grid_.yLines().back());
        const auto tolerance = options_.thermalBoundaryToleranceDbu;
        if (x < grid_.xLines().front()-tolerance || x > grid_.xLines().back()+tolerance
            || y < grid_.yLines().front()-tolerance || y > grid_.yLines().back()+tolerance)
            throw std::runtime_error("Thermal " + label + " XY coordinate is outside DIEAREA");
        const auto clippedX = std::clamp(x, static_cast<long double>(grid_.xLines().front()), static_cast<long double>(grid_.xLines().back()));
        const auto clippedY = std::clamp(y, static_cast<long double>(grid_.yLines().front()), static_cast<long double>(grid_.yLines().back()));
        if (x != clippedX || y != clippedY) ++next.clippedNodes;
        const auto cell = grid_.locate({static_cast<Coord>(std::llround(clippedX)), static_cast<Coord>(std::llround(clippedY))}, 0);
        const auto xy = static_cast<std::size_t>(cell.y) * grid_.width() + cell.x;
        const auto z = node[2];
        if (dies.insert(z).second) {
            if (dies.size() > static_cast<std::size_t>(grid_.layerCount()))
                throw std::runtime_error("Too many thermal die coordinates for the routing stack");
        }
        sums[xy] += node[3]; ++counts[xy];
    }
    if (const auto names = field("dieNames")) {
        kind(*names, Json::Kind::Array, "meta.dieNames");
        for (const auto& name : names->array) kind(name, Json::Kind::String, "meta.dieNames entry");
        if (names->array.size() != dies.size())
            throw std::runtime_error("Thermal dieNames count differs from the number of distinct z coordinates");
    }
    next.temperature.assign(xySize, std::numeric_limits<double>::quiet_NaN()); next.normalized.resize(xySize);
    for (std::size_t xy = 0; xy < xySize; ++xy) {
        if (counts[xy]) {
            next.temperature[xy] = static_cast<double>(sums[xy] / counts[xy]); ++next.sampledCells;
            if (!std::isfinite(next.temperature[xy])) throw std::runtime_error("Thermal average is not finite");
        }
    }
    // Match the evaluated public map: row forward-fill, then column forward-fill.
    // Leading holes use the first following sample, preserving constant fields.
    for (int y = 0; y < grid_.height(); ++y) {
        double previous = std::numeric_limits<double>::quiet_NaN();
        for (int x = 0; x < grid_.width(); ++x) {
            auto& value = next.temperature[static_cast<std::size_t>(y)*grid_.width()+x];
            if (std::isnan(value)) value = previous; else previous = value;
        }
        for (int x = grid_.width()-1; x >= 0; --x) {
            auto& value = next.temperature[static_cast<std::size_t>(y)*grid_.width()+x];
            if (std::isnan(value)) value = previous; else previous = value;
        }
    }
    for (int x = 0; x < grid_.width(); ++x) {
        double previous = std::numeric_limits<double>::quiet_NaN();
        for (int y = 0; y < grid_.height(); ++y) {
            auto& value = next.temperature[static_cast<std::size_t>(y)*grid_.width()+x];
            if (std::isnan(value)) value = previous; else previous = value;
        }
        for (int y = grid_.height()-1; y >= 0; --y) {
            auto& value = next.temperature[static_cast<std::size_t>(y)*grid_.width()+x];
            if (std::isnan(value)) value = previous; else previous = value;
        }
    }
    next.filledCells = xySize-next.sampledCells;
    if (next.filledCells) warnings.push_back("Sparse thermal map: filled " + std::to_string(next.filledCells) + " XY GCells");
    if (next.clippedNodes) warnings.push_back("Thermal boundary tolerance clipped " + std::to_string(next.clippedNodes) + " samples");
    const auto range = std::minmax_element(next.temperature.begin(), next.temperature.end());
    const long double low = *range.first, span = static_cast<long double>(*range.second) - low;
    if (span > 0) for (std::size_t xy = 0; xy < xySize; ++xy)
        next.normalized[xy] = std::clamp(static_cast<double>((static_cast<long double>(next.temperature[xy]) - low) / span), 0.0, 1.0);
    else warnings.push_back("Thermal map is spatially uniform; temperature-density correlation is zero");
    next.mappedNodes = nodeRecords.size(); next.loaded = true;
    thermal_ = std::move(next);
    warnings_.insert(warnings_.end(), warnings.begin(), warnings.end());
}

bool Resources::isCross(int fromLayer, int toLayer) const {
    return crossPairs_.count(pairKey(fromLayer, toLayer)) != 0;
}
bool Resources::allowed(Cell from, Cell to) const {
    return adjacent(grid_, from, to) && capacity_[grid_.id(from)] > 0 && capacity_[grid_.id(to)] > 0;
}
double Resources::stepCost(Cell from, Cell to) const {
    if (!allowed(from, to)) return std::numeric_limits<double>::infinity();
    const bool cross = isCross(from.layer, to.layer);
    const double base = baseCost(grid_, options_, thermal_, cross, from, to);
    const auto target = grid_.id(to);
    const double prospective = static_cast<double>(usage_[target]) + 1;
    const double utilization = prospective / capacity_[target];
    const double excess = std::max(0.0, prospective - capacity_[target]);
    double cost = base + options_.presentWeight * presentFactor_ * (1.0 + base)
                        * (utilization * utilization + excess * excess)
                       + options_.historyWeight * history_[target];
    if (cross) cost += options_.presentWeight * presentFactor_ * options_.crossViaCost
                      * tsv_[from.y * grid_.width() + from.x];
    return cost;
}

std::vector<int> Resources::routeNodes(const RoutedNet& route) const {
    std::vector<int> nodes;
    for (const auto& branch : route.branches) for (const auto cell : branch) nodes.push_back(grid_.id(cell));
    for (const auto& terminal : route.terminals) {
        nodes.push_back(grid_.id(terminal.cell));
        for (const auto cell : terminal.accessPath) nodes.push_back(grid_.id(cell));
        for (const auto layer : terminal.accessLayers)
            nodes.push_back(grid_.id({terminal.cell.x, terminal.cell.y, layer}));
    }
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    return nodes;
}
std::vector<std::pair<int, int>> Resources::routeEdges(const RoutedNet& route) const {
    std::vector<std::pair<int, int>> edges;
    const auto add = [&](Cell a, Cell b) {
        if (a == b) return;
        if (!adjacent(grid_, a, b)) throw std::runtime_error("Route contains a non-adjacent or unsupported VIA edge");
        edges.push_back(pairKey(grid_.id(a), grid_.id(b)));
    };
    for (const auto& branch : route.branches)
        for (std::size_t i = 1; i < branch.size(); ++i) add(branch[i - 1], branch[i]);
    for (const auto& terminal : route.terminals)
        for (std::size_t i = 1; i < terminal.accessLayers.size(); ++i)
            add({terminal.cell.x, terminal.cell.y, terminal.accessLayers[i - 1]},
                {terminal.cell.x, terminal.cell.y, terminal.accessLayers[i]});
    for (const auto& terminal : route.terminals)
        for (std::size_t i = 1; i < terminal.accessPath.size(); ++i)
            add(terminal.accessPath[i-1],terminal.accessPath[i]);
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}
void Resources::commit(const RoutedNet& route, int delta) {
    if (delta != 1 && delta != -1) throw std::runtime_error("Resource commit delta must be +1 or -1");
    if (!route.success || route.skipped) return;
    const auto nodes = routeNodes(route);
    const auto edges = routeEdges(route);
    std::map<int, int> crosses;
    for (const auto& edge : edges) {
        const auto a = grid_.cell(edge.first), b = grid_.cell(edge.second);
        if (isCross(a.layer, b.layer)) ++crosses[a.y * grid_.width() + a.x];
    }
    // All checks precede mutations: a failed decrement leaves both arrays intact.
    for (const int node : nodes) {
        const auto next = static_cast<long long>(usage_[node]) + delta;
        if (next < 0 || next > std::numeric_limits<int>::max()) throw std::runtime_error("Resource usage commit would underflow or overflow");
    }
    for (const auto& cross : crosses) {
        const auto next = static_cast<long long>(tsv_[cross.first]) + static_cast<long long>(delta) * cross.second;
        if (next < 0 || next > std::numeric_limits<int>::max()) throw std::runtime_error("TSV density commit would underflow or overflow");
    }
    for (const int node : nodes) usage_[node] += delta;
    for (const auto& cross : crosses) tsv_[cross.first] += delta * cross.second;
}
void Resources::clearUsage() { std::fill(usage_.begin(), usage_.end(), 0); std::fill(tsv_.begin(), tsv_.end(), 0); }
void Resources::updateHistory() {
    for (std::size_t i = 0; i < history_.size(); ++i)
        if (usage_[i] > capacity_[i])
            history_[i] = std::min(1.0e12, history_[i] + static_cast<double>(usage_[i] - capacity_[i]) / std::max(1, capacity_[i]));
}
void Resources::setPresentFactor(double factor) {
    if (!std::isfinite(factor) || factor < 0) throw std::runtime_error("Present congestion factor must be finite and nonnegative");
    presentFactor_ = factor;
}
long long Resources::overflow() const {
    long long result = 0;
    for (std::size_t i = 0; i < usage_.size(); ++i) result += std::max(0, usage_[i] - capacity_[i]);
    return result;
}
int Resources::maxOverflow() const {
    int result = 0;
    for (std::size_t i = 0; i < usage_.size(); ++i) result = std::max(result, usage_[i] - capacity_[i]);
    return result;
}
std::size_t Resources::overflowCells() const {
    std::size_t result = 0;
    for (std::size_t i = 0; i < usage_.size(); ++i) if (usage_[i] > capacity_[i]) ++result;
    return result;
}
std::size_t Resources::usedCells() const {
    return static_cast<std::size_t>(std::count_if(usage_.begin(), usage_.end(), [](int used) { return used > 0; }));
}
double Resources::correlation(bool aboveAmbient) const {
    if (!thermal_.loaded || tsv_.empty()) return 0;
    long double meanTemperature = 0, meanDensity = 0;
    const auto temperature = [&](std::size_t i) -> long double {
        return aboveAmbient ? std::max(0.0L, static_cast<long double>(thermal_.temperature[i]) - thermal_.ambient)
                            : static_cast<long double>(thermal_.temperature[i]);
    };
    // Center before summing so a uniform map has exactly zero variance,
    // including decimal temperatures whose repeated averages round slightly.
    const auto reference = temperature(0);
    for (std::size_t i = 0; i < tsv_.size(); ++i) {
        meanTemperature += temperature(i) - reference;
        meanDensity += tsv_[i];
    }
    meanTemperature /= tsv_.size();
    meanDensity /= tsv_.size();
    long double covariance = 0, temperatureVariance = 0, densityVariance = 0;
    for (std::size_t i = 0; i < tsv_.size(); ++i) {
        const long double a = (temperature(i) - reference) - meanTemperature, b = tsv_[i] - meanDensity;
        covariance += a * b; temperatureVariance += a * a; densityVariance += b * b;
    }
    if (!(temperatureVariance > 0) || !(densityVariance > 0)) return 0;
    const auto result = covariance / std::sqrt(temperatureVariance) / std::sqrt(densityVariance);
    return std::isfinite(result) ? std::clamp(static_cast<double>(result), -1.0, 1.0) : 0;
}
double Resources::staticCost(const RoutedNet& route) const {
    if (!route.success || route.skipped) return 0;
    double result = 0;
    for (const auto& edge : routeEdges(route)) {
        const auto a = grid_.cell(edge.first), b = grid_.cell(edge.second);
        result += baseCost(grid_, options_, thermal_, isCross(a.layer, b.layer), a, b);
    }
    return result;
}

} // namespace router::contest
