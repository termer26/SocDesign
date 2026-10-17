#pragma once
#include "router/types.hpp"
#include <stdexcept>

namespace router {

struct Token { std::string text; std::size_t begin = 0, end = 0; };
std::string readText(const std::string& path);
std::vector<Token> tokenize(const std::string& text);
Coord integer(const std::string& text);
double real(const std::string& text);

// 小型游标：所有 parser 共享词法分析，但各自只处理需要的语法。
class Cursor {
public:
    explicit Cursor(const std::vector<Token>& tokens) : tokens_(tokens) {}
    bool empty() const { return index_ >= tokens_.size(); }
    const Token& peek(std::size_t offset = 0) const;
    Token take();
    bool accept(const std::string& value);
    void expect(const std::string& value);
    std::vector<Token> statement(); // 读到 ;，包括 ; 本身。
private:
    const std::vector<Token>& tokens_;
    std::size_t index_ = 0;
};

} // namespace router
