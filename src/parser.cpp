#include "peck/compiler.hpp"
#include "lexer.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace peck {
namespace {

class Parser {
public:
    Parser(
        const std::string& source,
        std::filesystem::path source_path = {},
        std::unordered_set<std::string>* imported_paths = nullptr,
        bool require_main = true)
        : tokens_(tokenize(source)), source_path_(std::move(source_path)),
          imported_paths_(imported_paths), require_main_(require_main) {}

    Program parse() {
        Program program;
        while (!check(TokenKind::EndOfFile)) {
            if (check_identifier("struct")) {
                program.structs.push_back(parse_struct());
            } else if (check_identifier("numer")) {
                program.enums.push_back(parse_enum());
            } else if (check_identifier("change-method")) {
                program.meta_methods.push_back(parse_meta_method());
            } else if (check_identifier("pack") || check_identifier("import")) {
                auto imported = parse_import();
                program.structs.insert(program.structs.end(), imported.structs.begin(), imported.structs.end());
                program.enums.insert(program.enums.end(), imported.enums.begin(), imported.enums.end());
                program.meta_methods.insert(program.meta_methods.end(), imported.meta_methods.begin(), imported.meta_methods.end());
                program.functions.insert(program.functions.end(), imported.functions.begin(), imported.functions.end());
            } else if (check_identifier("Str") || check_identifier("func")) {
                program.functions.push_back(parse_function());
            } else {
                fail("expected enum, struct, import, meta-method, or function declaration");
            }
        }
        if (require_main_) {
            bool has_main = false;
            for (const auto& function : program.functions) has_main = has_main || function.name == "main";
            if (!has_main) fail("program must define main");
        }
        return program;
    }

private:
    bool check(TokenKind kind) const { return peek().kind == kind; }
    bool check_next(TokenKind kind) const {
        return position_ + 1 < tokens_.size() && tokens_[position_ + 1].kind == kind;
    }
    bool check_identifier(const std::string& text) const {
        return (check(TokenKind::Identifier) || check(TokenKind::Keyword)) && peek().text == text;
    }
    const Token& peek() const { return tokens_[position_]; }

    Token consume(TokenKind kind, const std::string& message) {
        if (!check(kind)) fail(message);
        return tokens_[position_++];
    }

    Token consume_identifier(const std::string& text, const std::string& message) {
        if (!check_identifier(text)) fail(message);
        return tokens_[position_++];
    }

    Token consume_name(const std::string& message) {
        return consume(TokenKind::Identifier, message);
    }

    static std::string read_source_file(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open imported source file: " + path.string());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    Program parse_import() {
        ++position_;
        const auto relative_path = consume(TokenKind::String, "expected quoted path after pack/import").text;
        auto imported_path = source_path_.empty()
            ? std::filesystem::path(relative_path)
            : source_path_.parent_path() / relative_path;
        std::error_code path_error;
        auto canonical_path = std::filesystem::weakly_canonical(imported_path, path_error);
        if (path_error) canonical_path = std::filesystem::absolute(imported_path);
        const auto key = canonical_path.string();
        if (imported_paths_ && !imported_paths_->insert(key).second) return {};
        auto imported_source = read_source_file(canonical_path);
        Parser imported_parser(imported_source, canonical_path, imported_paths_, false);
        return imported_parser.parse();
    }

    StructDeclASTNode parse_struct() {
        consume_identifier("struct", "expected 'struct'");
        StructDeclASTNode declaration;
        declaration.name = consume_name("expected struct name").text;
        consume(TokenKind::LeftBrace, "expected '{' after struct name");
        while (!check_identifier("End") && !check(TokenKind::EndOfFile)) {
            MutabilityMode mutability = MutabilityMode::Mutable;
            if (check(TokenKind::Keyword) && is_mutability_word(peek().text)) mutability = parse_mutability();
            if (check(TokenKind::Keyword) && is_type_word(peek().text)) ++position_;
            const auto field_name = consume_name("expected struct field name").text;
            const auto suffix = consume(TokenKind::TypeSuffix, "expected struct field type suffix such as !N");
            declaration.fields.push_back({field_name, type_from_suffix(suffix.text), mutability});
        }
        consume_identifier("End", "expected 'End' before '}' in struct declaration");
        consume(TokenKind::RightBrace, "expected '}' after struct fields");
        return declaration;
    }

    EnumDeclASTNode parse_enum() {
        consume_identifier("numer", "expected 'numer'");
        EnumDeclASTNode declaration;
        declaration.name = consume_name("expected enum name").text;
        consume(TokenKind::LeftBrace, "expected '{' after enum name");
        while (!check_identifier("End") && !check(TokenKind::EndOfFile)) {
            declaration.variants.push_back(consume_name("expected enum variant").text);
            if (!check(TokenKind::Comma)) break;
            consume(TokenKind::Comma, "expected ',' between enum variants");
        }
        consume_identifier("End", "expected 'End' before '}' in enum declaration");
        consume(TokenKind::RightBrace, "expected '}' after enum variants");
        return declaration;
    }

    CustomMetaMethodASTNode parse_meta_method() {
        consume_identifier("change-method", "expected 'change-method'");
        CustomMetaMethodASTNode method;
        method.name = consume_name("expected custom method name").text;
        consume(TokenKind::LeftBrace, "expected '{' before custom method body");
        while (!check_identifier("End") && !check(TokenKind::EndOfFile)) {
            auto operation = parse_expression(true);
            if (operation.kind != ExpressionASTNode::Kind::Call) {
                fail("custom meta-method body only accepts meta-method calls");
            }
            method.operations.push_back(std::move(operation));
        }
        consume_identifier("End", "expected 'End' before '}' in custom method");
        consume(TokenKind::RightBrace, "expected '}' after custom method body");
        return method;
    }

    static PeckType type_from_suffix(const std::string& suffix) {
        if (suffix == "N") return PeckType::Integer;
        if (suffix == "F") return PeckType::Float;
        if (suffix == "D") return PeckType::Double;
        if (suffix == "B") return PeckType::Byte;
        if (suffix == "C") return PeckType::Character;
        if (suffix == "L" || suffix == "str") return PeckType::String;
        throw std::runtime_error("unsupported function type suffix '!" + suffix + "'");
    }

    FunctionASTNode parse_function() {
        if (check_identifier("Str")) consume_identifier("Str", "expected 'Str'");
        else consume_identifier("func", "expected 'func'");
        FunctionASTNode function;
        function.name = consume_name("expected function name").text;
        consume(TokenKind::LeftParen, "expected '(' after function name");
        if (!check(TokenKind::RightParen)) {
            do {
                function.parameters.push_back(parse_parameter());
                if (!check(TokenKind::Comma)) break;
                consume(TokenKind::Comma, "expected ',' between parameters");
            } while (!check(TokenKind::RightParen));
        }
        consume(TokenKind::RightParen, "expected ')' after parameters");
        if (check(TokenKind::TypeSuffix)) {
            function.return_type = type_from_suffix(tokens_[position_++].text);
        }
        consume(TokenKind::LeftBrace, "expected '{' before function body");
        while (!check_identifier("End") && !check(TokenKind::EndOfFile)) {
            function.body.push_back(parse_statement());
        }
        consume_identifier("End", "expected 'End' before '}' in function body");
        consume(TokenKind::RightBrace, "expected '}' after End");
        return function;
    }

    ParameterASTNode parse_parameter() {
        MutabilityMode mutability = MutabilityMode::Immutable;
        if (check(TokenKind::Keyword)) {
            const auto& word = peek().text;
            if (word == "change" || word == "chn" || word == "alwchang" || word == "alwchn") {
                mutability = (word == "change" || word == "chn")
                    ? MutabilityMode::Mutable : MutabilityMode::AlwaysMutable;
                ++position_;
            } else if (word == "var" || word == "mut" || word == "const") {
                mutability = MutabilityMode::Immutable;
                ++position_;
            }
        }
        const auto name = consume_name("expected parameter name").text;
        const auto suffix = consume(TokenKind::TypeSuffix, "expected parameter type suffix such as !N");
        return {name, type_from_suffix(suffix.text), mutability};
    }

    OutputCallASTNode parse_output() {
        consume_identifier("Output", "expected 'Output'");
        consume(TokenKind::Dot, "expected '.' after Output");
        consume_identifier("string", "expected 'string' after Output.");
        consume(TokenKind::LeftParen, "expected '(' after Output.string");
        const auto text = consume(TokenKind::String, "expected string literal").text;
        consume(TokenKind::RightParen, "expected ')' after string literal");
        return {text};
    }

    StatementASTNode parse_statement() {
        if (check_identifier("Output")) return parse_output();
        if (check_identifier("free")) return CallStatementASTNode{parse_expression(true)};
        if (check_identifier("panic")) {
            consume_identifier("panic", "expected 'panic'");
            consume(TokenKind::LeftParen, "expected '(' after panic");
            const auto reason = consume(TokenKind::String, "expected panic diagnostic string").text;
            consume(TokenKind::RightParen, "expected ')' after panic diagnostic");
            return PanicASTNode{reason};
        }
        if (check_identifier("ret") && !check_next(TokenKind::Dot)) {
            consume_identifier("ret", "expected 'ret'");
            return ReturnASTNode{parse_expression(true)};
        }
        if (check_identifier("respon")) return parse_respon();
        if (check_identifier("if")) return parse_if();
        if (check_identifier("while")) return parse_loop(LoopKind::While);
        if (check_identifier("for")) return parse_loop(LoopKind::For);
        if ((check(TokenKind::Identifier) || check(TokenKind::Keyword)) && check_next(TokenKind::LeftParen)) {
            return CallStatementASTNode{parse_expression(true)};
        }
        if (is_assignment_start()) return parse_assignment();
        if ((check(TokenKind::Identifier) || check(TokenKind::Keyword)) && check_next(TokenKind::Dot)) {
            return CallStatementASTNode{parse_expression(true)};
        }
        return parse_declaration();
    }

    bool is_assignment_start() const {
        if (check(TokenKind::Identifier) && check_next(TokenKind::Equal)) return true;
        if (check(TokenKind::Asterisk) || check(TokenKind::Ampersand) ||
            check(TokenKind::Caret) || check(TokenKind::At)) return true;
        return check(TokenKind::Identifier) && check_next(TokenKind::Dot) &&
            position_ + 3 < tokens_.size() && tokens_[position_ + 2].kind == TokenKind::Identifier &&
            tokens_[position_ + 3].kind == TokenKind::Equal;
    }

    AssignmentASTNode parse_assignment() {
        auto target = parse_expression(true);
        consume(TokenKind::Equal, "expected '=' in assignment");
        return {std::move(target), parse_expression(true)};
    }

    ConditionASTNode parse_condition() {
        const bool parenthesized = check(TokenKind::LeftParen);
        if (parenthesized) consume(TokenKind::LeftParen, "expected '('");
        auto left = parse_expression(true);

        ComparisonOperator operation;
        switch (peek().kind) {
        case TokenKind::EqualEqual: operation = ComparisonOperator::Equal; break;
        case TokenKind::BangEqual: operation = ComparisonOperator::NotEqual; break;
        case TokenKind::Less: operation = ComparisonOperator::Less; break;
        case TokenKind::LessEqual: operation = ComparisonOperator::LessEqual; break;
        case TokenKind::Greater: operation = ComparisonOperator::Greater; break;
        case TokenKind::GreaterEqual: operation = ComparisonOperator::GreaterEqual; break;
        default: fail("expected comparison operator in if condition");
        }
        ++position_;
        auto right = parse_expression(true);
        if (parenthesized) consume(TokenKind::RightParen, "expected ')' after if condition");
        return {std::move(left), operation, std::move(right)};
    }

    std::vector<StatementASTNode> parse_braced_block() {
        consume(TokenKind::LeftBrace, "expected '{' before block");
        std::vector<StatementASTNode> statements;
        while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile) && !check_identifier("End")) {
            statements.push_back(parse_statement());
        }
        if (check_identifier("End")) consume_identifier("End", "expected optional 'End'");
        consume(TokenKind::RightBrace, "expected '}' after block");
        return statements;
    }

    IfASTNodePtr parse_if() {
        consume_identifier("if", "expected 'if'");
        auto node = std::make_shared<IfASTNode>();
        node->condition = parse_condition();
        node->then_branch = parse_braced_block();

        if (check_identifier("else")) {
            consume_identifier("else", "expected 'else'");
            if (check_identifier("if")) {
                node->else_branch.emplace_back(parse_if());
            } else {
                node->else_branch = parse_braced_block();
            }
        }
        return node;
    }

    ResponASTNodePtr parse_respon() {
        consume_identifier("respon", "expected 'respon'");
        auto node = std::make_shared<ResponASTNode>();
        node->variable = consume_name("expected enum variable after respon").text;
        consume(TokenKind::LeftBrace, "expected '{' before respon cases");
        while (!check_identifier("End") && !check(TokenKind::EndOfFile)) {
            ResponCaseASTNode case_node;
            case_node.enum_name = consume_name("expected enum type in respon case").text;
            consume(TokenKind::Dot, "expected '.' before enum variant");
            case_node.variant = consume_name("expected enum variant in respon case").text;
            case_node.body = parse_braced_block();
            node->cases.push_back(std::move(case_node));
        }
        consume_identifier("End", "expected 'End' before '}' after respon cases");
        consume(TokenKind::RightBrace, "expected '}' after respon cases");
        return node;
    }

    std::uint8_t parse_speed() {
        const auto speed_token = consume(TokenKind::Number, "expected loop speed from 0 to 50");
        std::uint64_t speed = 0;
        const auto* first = speed_token.text.data();
        const auto* last = first + speed_token.text.size();
        const auto result = std::from_chars(first, last, speed);
        if (result.ec != std::errc{} || result.ptr != last || speed > 50) {
            fail("loop speed must be an integer from 0 to 50");
        }
        return static_cast<std::uint8_t>(speed);
    }

    LoopASTNodePtr parse_loop(LoopKind kind) {
        consume_identifier(kind == LoopKind::While ? "while" : "for", "expected loop keyword");
        auto node = std::make_shared<LoopASTNode>();
        node->kind = kind;
        if (kind == LoopKind::For) node->initializer = parse_declaration();

        consume(TokenKind::LeftBrace, "expected '{' before loop body");
        while (!check_identifier("cond-end") && !check(TokenKind::EndOfFile)) {
            node->body.push_back(parse_statement());
        }
        consume_identifier("cond-end", "expected 'cond-end' at loop body end");
        consume(TokenKind::Equal, "expected '=' after cond-end");
        consume(TokenKind::LeftParen, "expected '(' after cond-end =");

        if (kind == LoopKind::While && check_identifier("true")) {
            consume_identifier("true", "expected boolean loop condition");
            node->repeat_forever = true;
            if (check(TokenKind::Comma)) {
                consume(TokenKind::Comma, "expected ',' before loop speed");
                node->speed = parse_speed();
            }
        } else if (kind == LoopKind::While) {
            node->condition = parse_condition_contents();
            if (check(TokenKind::Comma)) {
                consume(TokenKind::Comma, "expected ',' before loop speed");
                node->speed = parse_speed();
            }
        } else {
            node->iteration_limit = parse_expression(true);
            if (check(TokenKind::Comma)) {
                consume(TokenKind::Comma, "expected ',' before loop speed");
                node->speed = parse_speed();
            }
        }
        consume(TokenKind::RightParen, "expected ')' after cond-end expression");
        consume(TokenKind::RightBrace, "expected '}' after cond-end expression");
        return node;
    }

    static bool is_mutability_word(const std::string& word) {
        return word == "change" || word == "chn" || word == "alwchang" || word == "alwchn" ||
            word == "const" || word == "mut" || word == "mut-t" || word == "mut-time";
    }

    static bool is_type_word(const std::string& word) {
        return word == "var" || word == "bt" || word == "data" || word == "char" ||
            word == "line" || word == "array";
    }

    static PeckType type_from_word(const std::string& word) {
        if (word == "bt") return PeckType::Byte;
        if (word == "data") return PeckType::Data;
        if (word == "char") return PeckType::Character;
        if (word == "line") return PeckType::String;
        if (word == "array") return PeckType::Array;
        return PeckType::Inferred;
    }

    MutabilityMode parse_mutability() {
        const auto keyword = tokens_[position_++].text;
        if (keyword == "change" || keyword == "chn") return MutabilityMode::Mutable;
        if (keyword == "alwchang" || keyword == "alwchn") return MutabilityMode::AlwaysMutable;
        if (keyword == "const") return MutabilityMode::Constant;
        if (keyword == "mut") return MutabilityMode::Immutable;
        return MutabilityMode::Temporary;
    }

    std::optional<std::uint64_t> parse_ticks() {
        if (!check(TokenKind::LeftBracket)) return std::nullopt;
        consume(TokenKind::LeftBracket, "expected '[' before tick duration");
        const auto tick = consume(TokenKind::Identifier, "expected tk-<number> tick duration");
        consume(TokenKind::RightBracket, "expected ']' after tick duration");
        if (tick.text.rfind("tk-", 0) != 0 || tick.text.size() == 3) {
            fail("tick duration must use the form tk-<number>");
        }
        std::uint64_t value = 0;
        const auto* first = tick.text.data() + 3;
        const auto* last = tick.text.data() + tick.text.size();
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{} || result.ptr != last) fail("invalid tick duration");
        return value;
    }

    ExpressionASTNode parse_expression(bool allow_identifier = false) {
        auto expression = parse_primary(allow_identifier);
        while (check(TokenKind::Plus)) {
            consume(TokenKind::Plus, "expected '+'");
            auto combined = ExpressionASTNode{ExpressionASTNode::Kind::Add, "+"};
            combined.left = std::make_shared<ExpressionASTNode>(std::move(expression));
            combined.right = std::make_shared<ExpressionASTNode>(parse_primary(allow_identifier));
            expression = std::move(combined);
        }
        return expression;
    }

    ExpressionASTNode parse_primary(bool allow_identifier) {
        if (check(TokenKind::Number)) return {ExpressionASTNode::Kind::Number, tokens_[position_++].text};
        if (check(TokenKind::String)) return {ExpressionASTNode::Kind::String, tokens_[position_++].text};
        if (check(TokenKind::Character)) return {ExpressionASTNode::Kind::Character, tokens_[position_++].text};
        if (check(TokenKind::Ampersand) || check(TokenKind::Asterisk) ||
            check(TokenKind::Caret) || check(TokenKind::At)) {
            PointerExprASTNode::Operation operation;
            ExpressionASTNode::Kind kind;
            if (check(TokenKind::Ampersand)) {
                operation = PointerExprASTNode::Operation::AddressOf;
                kind = ExpressionASTNode::Kind::AddressOf;
            } else if (check(TokenKind::Asterisk)) {
                operation = PointerExprASTNode::Operation::Dereference;
                kind = ExpressionASTNode::Kind::Dereference;
            } else if (check(TokenKind::Caret)) {
                operation = PointerExprASTNode::Operation::Up;
                kind = ExpressionASTNode::Kind::PointerUp;
            } else {
                operation = PointerExprASTNode::Operation::At;
                kind = ExpressionASTNode::Kind::PointerAt;
            }
            ++position_;
            auto operand = std::make_shared<ExpressionASTNode>(parse_primary(allow_identifier));
            ExpressionASTNode pointer_expression{kind};
            pointer_expression.pointer = std::make_shared<PointerExprASTNode>(
                PointerExprASTNode{operation, std::move(operand)});
            return pointer_expression;
        }
        if (allow_identifier && (check_identifier("select") || check_identifier("free"))) {
            const auto is_select = check_identifier("select");
            ++position_;
            consume(TokenKind::LeftParen, "expected '(' after memory operation");
            if (is_select) {
                const auto type_name = consume_name("expected struct type in select()").text;
                consume(TokenKind::RightParen, "expected ')' after select type");
                ExpressionASTNode selection{ExpressionASTNode::Kind::Select, type_name};
                return selection;
            }
            auto operand = std::make_shared<ExpressionASTNode>(parse_expression(true));
            consume(TokenKind::RightParen, "expected ')' after free argument");
            ExpressionASTNode release{ExpressionASTNode::Kind::Free, "free"};
            release.left = std::move(operand);
            return release;
        }
        if (allow_identifier && (check(TokenKind::Identifier) || check(TokenKind::Keyword))) {
            const auto base = tokens_[position_++].text;
            std::vector<std::string> members;
            while (check(TokenKind::Dot)) {
                consume(TokenKind::Dot, "expected '.' before member name");
                members.push_back(consume_name("expected member or meta-method name after '.'").text);
            }
            if (check(TokenKind::LeftParen)) {
                consume(TokenKind::LeftParen, "expected '(' before call arguments");
                if (members.empty() && (base == "input" || base == "line")) {
                    consume(TokenKind::RightParen, "expected ')' after input call");
                    return {base == "input" ? ExpressionASTNode::Kind::ReadInput : ExpressionASTNode::Kind::ReadLine, base};
                }
                ExpressionASTNode call{ExpressionASTNode::Kind::Call, base};
                for (const auto& member : members) call.value += "." + member;
                if (!check(TokenKind::RightParen)) {
                    do {
                        call.arguments.push_back(parse_expression(true));
                        if (!check(TokenKind::Comma)) break;
                        consume(TokenKind::Comma, "expected ',' between call arguments");
                    } while (!check(TokenKind::RightParen));
                }
                consume(TokenKind::RightParen, "expected ')' after call arguments");
                return call;
            }

            ExpressionASTNode expression{ExpressionASTNode::Kind::Identifier, base};
            for (const auto& member : members) {
                ExpressionASTNode access{ExpressionASTNode::Kind::MemberAccess, member};
                access.member = std::make_shared<MemberAccessASTNode>(MemberAccessASTNode{
                    std::make_shared<ExpressionASTNode>(std::move(expression)), member});
                expression = std::move(access);
            }
            return expression;
        }
        fail("expected a numeric, string, or character literal");
    }

    ConditionASTNode parse_condition_contents() {
        auto left = parse_expression(true);
        ComparisonOperator operation;
        switch (peek().kind) {
        case TokenKind::EqualEqual: operation = ComparisonOperator::Equal; break;
        case TokenKind::BangEqual: operation = ComparisonOperator::NotEqual; break;
        case TokenKind::Less: operation = ComparisonOperator::Less; break;
        case TokenKind::LessEqual: operation = ComparisonOperator::LessEqual; break;
        case TokenKind::Greater: operation = ComparisonOperator::Greater; break;
        case TokenKind::GreaterEqual: operation = ComparisonOperator::GreaterEqual; break;
        default: fail("expected comparison operator in loop condition");
        }
        ++position_;
        auto right = parse_expression(true);
        return {std::move(left), operation, std::move(right)};
    }

    ArrayLiteralASTNode parse_array_literal() {
        consume(TokenKind::LeftBrace, "expected '{' to start array literal");
        ArrayLiteralASTNode array;
        if (!check(TokenKind::RightBrace)) {
            do {
                array.elements.push_back(parse_expression());
                if (!check(TokenKind::Comma)) break;
                consume(TokenKind::Comma, "expected ',' between array elements");
            } while (!check(TokenKind::RightBrace));
        }
        consume(TokenKind::RightBrace, "expected '}' after array elements");
        return array;
    }

    VarDeclASTNode parse_declaration() {
        VarDeclASTNode declaration;
        bool temporary = false;
        if (check(TokenKind::Keyword) && is_mutability_word(peek().text)) {
            declaration.mutability = parse_mutability();
            temporary = declaration.mutability == MutabilityMode::Temporary;
        }

        if (check(TokenKind::Keyword) && is_type_word(peek().text)) {
            const auto type_word = tokens_[position_++].text;
            declaration.explicit_type = type_from_word(type_word);
        }

        declaration.name = consume_name("expected variable name in declaration").text;
        if (temporary) {
            declaration.freeze_ticks = parse_ticks();
            if (!declaration.freeze_ticks) fail("temporary mutability requires [tk-<number>]");
        }

        if (check(TokenKind::TypeSuffix)) {
            declaration.type_suffix = tokens_[position_++].text;
            static const std::vector<std::string> supported = {"N", "F", "D", "B", "C", "L", "str"};
            bool found = false;
            for (const auto& suffix : supported) found = found || declaration.type_suffix == suffix;
            if (!found) fail("unsupported type suffix '!" + declaration.type_suffix + "'");
        }

        consume(TokenKind::Equal, "expected '=' before variable initializer");
        if (check(TokenKind::LeftBrace)) {
            declaration.initializer = parse_array_literal();
        } else {
            declaration.initializer = parse_expression(true);
        }
        if (declaration.explicit_type == PeckType::Array &&
            !std::holds_alternative<ArrayLiteralASTNode>(declaration.initializer)) {
            fail("array declaration requires an array literal initializer");
        }
        if (declaration.explicit_type == PeckType::Data && declaration.type_suffix.empty() &&
            !std::holds_alternative<ArrayLiteralASTNode>(declaration.initializer) &&
            std::get<ExpressionASTNode>(declaration.initializer).kind != ExpressionASTNode::Kind::String) {
            fail("data declaration requires a string or byte-array initializer");
        }
        return declaration;
    }

    [[noreturn]] void fail(const std::string& message) const {
        const auto& token = peek();
        throw std::runtime_error(std::to_string(token.line) + ":" + std::to_string(token.column) + ": " + message);
    }

    std::vector<Token> tokens_;
    std::size_t position_ = 0;
    std::filesystem::path source_path_;
    std::unordered_set<std::string>* imported_paths_ = nullptr;
    bool require_main_ = true;
};

}

Program parse_source(const std::string& source) {
    return Parser(source).parse();
}

Program parse_file(const std::filesystem::path& path) {
    std::unordered_set<std::string> imported_paths;
    std::error_code path_error;
    auto canonical_path = std::filesystem::weakly_canonical(path, path_error);
    if (path_error) canonical_path = std::filesystem::absolute(path);
    imported_paths.insert(canonical_path.string());
    std::ifstream input(canonical_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open source file: " + canonical_path.string());
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return Parser(source, canonical_path, &imported_paths, true).parse();
}

}