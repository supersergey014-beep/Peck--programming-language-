#include "lexer.hpp"

#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace hypha {
namespace {

bool is_keyword(const std::string& word) {
    static const std::vector<std::string> keywords = {
        "Str", "End", "Output", "var", "line", "bt", "data", "change", "chn",
        "alwchang", "alwchn", "const", "mut", "mut-t", "mut-time", "array", "char",
        "if", "else", "while", "for", "cond-end", "true", "ret", "func",
        "struct", "select", "free", "pack", "import",
        "numer", "respon", "change-method", "panic", "input",
    };
    for (const auto& keyword : keywords) {
        if (word == keyword) return true;
    }
    return false;
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    std::vector<Token> scan() {
        std::vector<Token> tokens;
        while (!at_end()) {
            skip_trivia();
            if (at_end()) break;

            const auto start_line = line_;
            const auto start_column = column_;
            const char character = advance();
            switch (character) {
            case '(': tokens.push_back({TokenKind::LeftParen, "(", start_line, start_column}); break;
            case ')': tokens.push_back({TokenKind::RightParen, ")", start_line, start_column}); break;
            case '{': tokens.push_back({TokenKind::LeftBrace, "{", start_line, start_column}); break;
            case '}': tokens.push_back({TokenKind::RightBrace, "}", start_line, start_column}); break;
            case '[': tokens.push_back({TokenKind::LeftBracket, "[", start_line, start_column}); break;
            case ']': tokens.push_back({TokenKind::RightBracket, "]", start_line, start_column}); break;
            case '.': tokens.push_back({TokenKind::Dot, ".", start_line, start_column}); break;
            case ',': tokens.push_back({TokenKind::Comma, ",", start_line, start_column}); break;
            case '+': tokens.push_back({TokenKind::Plus, "+", start_line, start_column}); break;
            case '&': tokens.push_back({TokenKind::Ampersand, "&", start_line, start_column}); break;
            case '*': tokens.push_back({TokenKind::Asterisk, "*", start_line, start_column}); break;
            case '^': tokens.push_back({TokenKind::Caret, "^", start_line, start_column}); break;
            case '@': tokens.push_back({TokenKind::At, "@", start_line, start_column}); break;
            case '=':
                tokens.push_back(match('=')
                    ? Token{TokenKind::EqualEqual, "==", start_line, start_column}
                    : Token{TokenKind::Equal, "=", start_line, start_column});
                break;
            case '!':
                if (match('=')) tokens.push_back({TokenKind::BangEqual, "!=", start_line, start_column});
                else tokens.push_back(type_suffix(start_line, start_column));
                break;
            case '<':
                tokens.push_back(match('=')
                    ? Token{TokenKind::LessEqual, "<=", start_line, start_column}
                    : Token{TokenKind::Less, "<", start_line, start_column});
                break;
            case '>':
                tokens.push_back(match('=')
                    ? Token{TokenKind::GreaterEqual, ">=", start_line, start_column}
                    : Token{TokenKind::Greater, ">", start_line, start_column});
                break;
            case '"': tokens.push_back(string_token(start_line, start_column)); break;
            case '\'': tokens.push_back(character_token(start_line, start_column)); break;
            default:
                if (std::isdigit(static_cast<unsigned char>(character)) ||
                    (character == '-' && !at_end() && std::isdigit(static_cast<unsigned char>(source_[position_])))) {
                    tokens.push_back(number_token(start_line, start_column));
                } else if (std::isalpha(static_cast<unsigned char>(character)) || character == '_') {
                    tokens.push_back(identifier_token(start_line, start_column));
                } else {
                    fail(start_line, start_column, "unexpected character");
                }
            }
        }
        tokens.push_back({TokenKind::EndOfFile, "", line_, column_});
        return tokens;
    }

private:
    bool at_end() const { return position_ >= source_.size(); }

    bool match(char expected) {
        if (at_end() || source_[position_] != expected) return false;
        advance();
        return true;
    }

    char advance() {
        const char character = source_[position_++];
        if (character == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return character;
    }

    void skip_trivia() {
        for (;;) {
            while (!at_end() && std::isspace(static_cast<unsigned char>(source_[position_]))) advance();
            if (position_ + 1 < source_.size() && source_[position_] == '/' && source_[position_ + 1] == '/') {
                while (!at_end() && advance() != '\n') {}
                continue;
            }
            return;
        }
    }

    Token identifier_token(std::size_t start_line, std::size_t start_column) {
        const auto start = position_ - 1;
        while (!at_end()) {
            const auto character = static_cast<unsigned char>(source_[position_]);
            const bool hyphenated = character == '-' && position_ + 1 < source_.size() &&
                (std::isalnum(static_cast<unsigned char>(source_[position_ + 1])) || source_[position_ + 1] == '_');
            if (!std::isalnum(character) && character != '_' && !hyphenated) break;
            advance();
        }
        auto word = std::string(source_.substr(start, position_ - start));
        const auto kind = is_keyword(word) ? TokenKind::Keyword : TokenKind::Identifier;
        return {kind, std::move(word), start_line, start_column};
    }

    Token number_token(std::size_t start_line, std::size_t start_column) {
        const auto start = position_ - 1;
        while (!at_end() && std::isdigit(static_cast<unsigned char>(source_[position_]))) advance();
        if (!at_end() && source_[position_] == '.' && position_ + 1 < source_.size() &&
            std::isdigit(static_cast<unsigned char>(source_[position_ + 1]))) {
            advance();
            while (!at_end() && std::isdigit(static_cast<unsigned char>(source_[position_]))) advance();
        }
        return {TokenKind::Number, std::string(source_.substr(start, position_ - start)), start_line, start_column};
    }

    Token type_suffix(std::size_t start_line, std::size_t start_column) {
        const auto start = position_;
        while (!at_end() && std::isalpha(static_cast<unsigned char>(source_[position_]))) advance();
        if (start == position_) fail(start_line, start_column, "expected a type name after '!'");
        return {TokenKind::TypeSuffix, std::string(source_.substr(start, position_ - start)), start_line, start_column};
    }

    char escaped_character(std::size_t start_line, std::size_t start_column) {
        if (at_end()) fail(start_line, start_column, "unterminated character literal");
        const char character = advance();
        if (character != '\\') return character;
        if (at_end()) fail(start_line, start_column, "unterminated escape sequence");
        switch (advance()) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        default: fail(line_, column_, "unsupported escape sequence");
        }
    }

    Token character_token(std::size_t start_line, std::size_t start_column) {
        const char value = escaped_character(start_line, start_column);
        if (at_end() || advance() != '\'') fail(start_line, start_column, "character literal must contain exactly one byte");
        return {TokenKind::Character, std::string(1, value), start_line, start_column};
    }

    Token string_token(std::size_t start_line, std::size_t start_column) {
        std::string value;
        while (!at_end() && source_[position_] != '"') {
            const char character = advance();
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (at_end()) fail(start_line, start_column, "unterminated escape sequence");
            switch (advance()) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '\\': value.push_back('\\'); break;
            case '"': value.push_back('"'); break;
            default: fail(line_, column_, "unsupported escape sequence");
            }
        }
        if (at_end()) fail(start_line, start_column, "unterminated string literal");
        advance();
        return {TokenKind::String, std::move(value), start_line, start_column};
    }

    [[noreturn]] void fail(std::size_t line, std::size_t column, const std::string& message) const {
        throw std::runtime_error(std::to_string(line) + ":" + std::to_string(column) + ": " + message);
    }

    std::string_view source_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

}

std::vector<Token> tokenize(const std::string& source) {
    return Lexer(source).scan();
}

}