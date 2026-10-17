#include "router/tokenizer.hpp"
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace router {

std::string readText(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read file: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Read failed: " + path);
    return buffer.str();
}

std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> result;
    std::size_t i = 0;
    // 可选 UTF-8 BOM。
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < text.size()) {
        const char ch = text[i];
        if (std::isspace(static_cast<unsigned char>(ch))) { ++i; continue; }
        if (ch == '#') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        const auto begin = i;
        if (ch == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                ++i;
            }
            if (i == text.size()) throw std::runtime_error("Unterminated quoted string");
            ++i;
        } else if (ch == '\\') {
            // DEF 的转义标识符以空白结尾，其中可以有括号、加号。
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        } else if (ch == '(' || ch == ')' || ch == ';' || ch == '+') {
            ++i;
        } else {
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))
                   && text[i] != '(' && text[i] != ')' && text[i] != ';'
                   && text[i] != '+' && text[i] != '#') ++i;
        }
        result.push_back({text.substr(begin, i - begin), begin, i});
    }
    return result;
}

Coord integer(const std::string& text) {
    std::size_t used = 0;
    const auto value = std::stoll(text, &used);
    if (used != text.size()) throw std::runtime_error("Expected integer: " + text);
    return value;
}
double real(const std::string& text) {
    std::size_t used = 0;
    const auto value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value))
        throw std::runtime_error("Expected finite number: " + text);
    return value;
}
const Token& Cursor::peek(std::size_t offset) const {
    if (index_ + offset >= tokens_.size()) throw std::runtime_error("Unexpected end of file");
    return tokens_[index_ + offset];
}
Token Cursor::take() { const auto token = peek(); ++index_; return token; }
bool Cursor::accept(const std::string& value) {
    if (!empty() && peek().text == value) { ++index_; return true; }
    return false;
}
void Cursor::expect(const std::string& value) {
    const auto token = take();
    if (token.text != value)
        throw std::runtime_error("Expected '" + value + "', got '" + token.text
                                 + "' at byte " + std::to_string(token.begin));
}
std::vector<Token> Cursor::statement() {
    std::vector<Token> result;
    do { result.push_back(take()); } while (result.back().text != ";");
    return result;
}

} // namespace router
