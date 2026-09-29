#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hypha {

enum class HyphaType {
    Inferred,
    Integer,
    Float,
    Double,
    Byte,
    Data,
    Character,
    String,
    Array,
    Pointer,
    Struct,
    Enum,
};

enum class MutabilityMode {
    Mutable,
    AlwaysMutable,
    Constant,
    Immutable,
    Temporary,
};

struct PointerExprASTNode;
struct MemberAccessASTNode;

struct ExpressionASTNode {
    enum class Kind {
        Number, String, Character, Identifier, Boolean, Add, Call, AddressOf,
        Dereference, PointerUp, PointerAt, Select, Free, MemberAccess,
        ReadInput, ReadLine,
    };
    Kind kind;
    std::string value;
    std::shared_ptr<ExpressionASTNode> left;
    std::shared_ptr<ExpressionASTNode> right;
    std::vector<ExpressionASTNode> arguments;
    std::shared_ptr<PointerExprASTNode> pointer;
    std::shared_ptr<MemberAccessASTNode> member;
};

struct PointerExprASTNode {
    enum class Operation { AddressOf, Dereference, Up, At };
    Operation operation;
    std::shared_ptr<ExpressionASTNode> operand;
};

struct MemberAccessASTNode {
    std::shared_ptr<ExpressionASTNode> object;
    std::string member;
};

struct ArrayLiteralASTNode {
    std::vector<ExpressionASTNode> elements;
};

using InitializerASTNode = std::variant<ExpressionASTNode, ArrayLiteralASTNode>;

struct VarDeclASTNode {
    std::string name;
    MutabilityMode mutability = MutabilityMode::Mutable;
    std::optional<HyphaType> explicit_type;
    std::string type_suffix;
    std::optional<std::uint64_t> freeze_ticks;
    InitializerASTNode initializer;
};

struct OutputCallASTNode {
    std::string text;
};

enum class ComparisonOperator { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct ConditionASTNode {
    ExpressionASTNode left;
    ComparisonOperator operation;
    ExpressionASTNode right;
};

struct AssignmentASTNode {
    ExpressionASTNode target;
    ExpressionASTNode value;
};

struct ReturnASTNode {
    std::optional<ExpressionASTNode> value;
};

struct PanicASTNode {
    std::string reason;
};

struct CallStatementASTNode {
    ExpressionASTNode call;
};

struct StructFieldASTNode {
    std::string name;
    HyphaType type;
    MutabilityMode mutability;
};

struct StructDeclASTNode {
    std::string name;
    std::vector<StructFieldASTNode> fields;
};

struct EnumDeclASTNode {
    std::string name;
    std::vector<std::string> variants;
};

struct CustomMetaMethodASTNode {
    std::string name;
    std::vector<ExpressionASTNode> operations;
};

struct IfASTNode;
using IfASTNodePtr = std::shared_ptr<IfASTNode>;
struct LoopASTNode;
using LoopASTNodePtr = std::shared_ptr<LoopASTNode>;
struct ResponASTNode;
using ResponASTNodePtr = std::shared_ptr<ResponASTNode>;
using StatementASTNode = std::variant<VarDeclASTNode, OutputCallASTNode, AssignmentASTNode, ReturnASTNode, PanicASTNode, CallStatementASTNode, IfASTNodePtr, LoopASTNodePtr, ResponASTNodePtr>;

struct IfASTNode {
    ConditionASTNode condition;
    std::vector<StatementASTNode> then_branch;
    std::vector<StatementASTNode> else_branch;
};

struct ResponCaseASTNode {
    std::string enum_name;
    std::string variant;
    std::vector<StatementASTNode> body;
};

struct ResponASTNode {
    std::string variable;
    std::vector<ResponCaseASTNode> cases;
};

enum class LoopKind { While, For };

struct LoopASTNode {
    LoopKind kind;
    std::optional<VarDeclASTNode> initializer;
    std::vector<StatementASTNode> body;
    std::optional<ConditionASTNode> condition;
    std::optional<ExpressionASTNode> iteration_limit;
    bool repeat_forever = false;
    std::optional<std::uint8_t> speed;
};

struct ParameterASTNode {
    std::string name;
    HyphaType type;
    MutabilityMode mutability;
};

struct FunctionASTNode {
    std::string name;
    std::vector<ParameterASTNode> parameters;
    std::optional<HyphaType> return_type;
    std::vector<StatementASTNode> body;
};

struct Program {
    std::vector<StructDeclASTNode> structs;
    std::vector<EnumDeclASTNode> enums;
    std::vector<CustomMetaMethodASTNode> meta_methods;
    std::vector<FunctionASTNode> functions;
};

Program parse_source(const std::string& source);
Program parse_file(const std::filesystem::path& path);
void emit_object(const Program& program, const std::filesystem::path& path);

}