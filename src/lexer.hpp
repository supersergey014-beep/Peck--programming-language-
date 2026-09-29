#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace hypha {

enum class TokenKind {
    Identifier,
    Keyword,
    String,
    Character,
    Number,
    TypeSuffix,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Dot,
    Comma,
    Equal,
    Plus,
    Ampersand,
    Asterisk,
    Caret,
    At,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    EndOfFile,
};

struct Token {
    TokenKind kind;
    std::string text;
    std::size_t line;
    std::size_t column;
};

std::vector<Token> tokenize(const std::string& source);

}