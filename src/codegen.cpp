#include "peck/compiler.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/IR/LegacyPassManager.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

namespace peck {
namespace {

struct VariableSymbol {
    llvm::Value* address;
    PeckType type;
    MutabilityMode mutability;
    llvm::Type* value_type = nullptr;
    PeckType pointee_type = PeckType::Inferred;
    std::string struct_name;
    MutabilityMode pointee_mutability = MutabilityMode::Mutable;
    std::string enum_name;
};

using SymbolTable = std::unordered_map<std::string, VariableSymbol>;

struct StructInfo {
    llvm::StructType* type;
    std::vector<StructFieldASTNode> fields;
};

struct EnumInfo {
    std::vector<std::string> variants;
};

struct FunctionInfo {
    llvm::Function* value;
    std::optional<PeckType> return_type;
    std::vector<ParameterASTNode> parameters;
    bool is_entry_point = false;
};

struct FunctionTable : std::unordered_map<std::string, FunctionInfo> {
    std::unordered_map<std::string, StructInfo> structs;
    std::unordered_map<std::string, EnumInfo> enums;
    std::unordered_map<std::string, CustomMetaMethodASTNode> meta_methods;
};

struct LValueInfo {
    llvm::Value* address;
    PeckType type;
    MutabilityMode mutability;
    llvm::Type* llvm_type;
    std::string struct_name;
};

PeckType suffix_type(const std::string& suffix) {
    if (suffix == "N") return PeckType::Integer;
    if (suffix == "F") return PeckType::Float;
    if (suffix == "D") return PeckType::Double;
    if (suffix == "B") return PeckType::Byte;
    if (suffix == "C") return PeckType::Character;
    if (suffix == "L" || suffix == "str") return PeckType::String;
    throw std::runtime_error("unsupported type suffix '!" + suffix + "'");
}

PeckType infer_type(const ExpressionASTNode& expression) {
    if (expression.kind == ExpressionASTNode::Kind::String) return PeckType::String;
    if (expression.kind == ExpressionASTNode::Kind::Character) return PeckType::Character;
    if (expression.kind == ExpressionASTNode::Kind::AddressOf ||
        expression.kind == ExpressionASTNode::Kind::PointerAt ||
        expression.kind == ExpressionASTNode::Kind::Select) return PeckType::Pointer;
    if (expression.kind == ExpressionASTNode::Kind::ReadInput ||
        expression.kind == ExpressionASTNode::Kind::ReadLine) return PeckType::String;
    if (expression.kind == ExpressionASTNode::Kind::MemberAccess) return PeckType::Enum;
    if (expression.kind == ExpressionASTNode::Kind::Identifier) {
        throw std::runtime_error("cannot infer a declaration type from an identifier initializer");
    }
    if (expression.kind == ExpressionASTNode::Kind::Add && expression.left && expression.right) {
        const auto left_type = infer_type(*expression.left);
        const auto right_type = infer_type(*expression.right);
        if (left_type == PeckType::Double || right_type == PeckType::Double) return PeckType::Double;
        if (left_type == PeckType::Float || right_type == PeckType::Float) return PeckType::Float;
        return PeckType::Integer;
    }
    return expression.value.find('.') == std::string::npos ? PeckType::Integer : PeckType::Double;
}

PeckType resolve_type(const VarDeclASTNode& declaration) {
    const bool is_array = std::holds_alternative<ArrayLiteralASTNode>(declaration.initializer);
    if (declaration.explicit_type == PeckType::Data &&
        (declaration.type_suffix.empty() || declaration.type_suffix == "D")) {
        return PeckType::Data;
    }
    if (is_array) return PeckType::Array;
    if (!declaration.type_suffix.empty()) return suffix_type(declaration.type_suffix);
    if (declaration.explicit_type && *declaration.explicit_type != PeckType::Inferred) {
        return *declaration.explicit_type;
    }
    return infer_type(std::get<ExpressionASTNode>(declaration.initializer));
}

llvm::Type* llvm_scalar_type(PeckType type, llvm::LLVMContext& context) {
    switch (type) {
    case PeckType::Integer: return llvm::Type::getInt32Ty(context);
    case PeckType::Float: return llvm::Type::getFloatTy(context);
    case PeckType::Double: return llvm::Type::getDoubleTy(context);
    case PeckType::Byte:
    case PeckType::Character: return llvm::Type::getInt8Ty(context);
    case PeckType::String: return llvm::PointerType::getUnqual(context);
    case PeckType::Pointer: return llvm::PointerType::getUnqual(context);
    case PeckType::Enum: return llvm::Type::getInt32Ty(context);
    default: throw std::runtime_error("type is not a scalar LLVM type");
    }
}

std::int64_t parse_integer(const std::string& text) {
    std::size_t consumed = 0;
    const auto value = std::stoll(text, &consumed, 10);
    if (consumed != text.size()) throw std::runtime_error("invalid integer literal: " + text);
    return value;
}

llvm::Value* scalar_value(
    llvm::IRBuilder<>& builder,
    const ExpressionASTNode& expression,
    PeckType type) {
    auto& context = builder.getContext();
    switch (type) {
    case PeckType::Integer: {
        if (expression.kind != ExpressionASTNode::Kind::Number || expression.value.find('.') != std::string::npos) {
            throw std::runtime_error("integer variable requires an integer literal");
        }
        const auto value = parse_integer(expression.value);
        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("integer literal is outside the i32 range");
        }
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), value, true);
    }
    case PeckType::Byte:
    case PeckType::Character: {
        std::int64_t value = 0;
        if (expression.kind == ExpressionASTNode::Kind::Character && expression.value.size() == 1) {
            value = static_cast<unsigned char>(expression.value[0]);
        } else if (type == PeckType::Byte && expression.kind == ExpressionASTNode::Kind::Number &&
                   expression.value.find('.') == std::string::npos) {
            value = parse_integer(expression.value);
        } else {
            throw std::runtime_error("byte/character variable requires a byte-sized literal");
        }
        if (value < 0 || value > std::numeric_limits<std::uint8_t>::max()) {
            throw std::runtime_error("byte literal is outside the i8 range");
        }
        return llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), value);
    }
    case PeckType::Float:
    case PeckType::Double: {
        if (expression.kind != ExpressionASTNode::Kind::Number) {
            throw std::runtime_error("floating-point variable requires a numeric literal");
        }
        const auto value = std::stod(expression.value);
        return llvm::ConstantFP::get(llvm_scalar_type(type, context), value);
    }
    case PeckType::String:
        if (expression.kind != ExpressionASTNode::Kind::String) {
            throw std::runtime_error("string variable requires a string literal");
        }
        return builder.CreateGlobalStringPtr(expression.value);
    default:
        throw std::runtime_error("unsupported scalar initializer type");
    }
}

llvm::Value* expression_value(
    llvm::IRBuilder<>& builder,
    const ExpressionASTNode& expression,
    const SymbolTable& symbols,
    const FunctionTable& functions,
    PeckType expected_type);

llvm::Value* emit_input_line(llvm::IRBuilder<>& builder) {
    auto* module = builder.GetInsertBlock()->getModule();
    auto& context = builder.getContext();
    auto* getchar_type = llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto* getchar_function = module->getFunction("getchar");
    if (!getchar_function) {
        getchar_function = llvm::Function::Create(
            getchar_type, llvm::Function::ExternalLinkage, "getchar", module);
    }

    auto* buffer_type = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 4096);
    auto* buffer = builder.CreateAlloca(buffer_type, nullptr, "input.buffer");
    auto* index_address = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "input.index");
    builder.CreateStore(builder.getInt32(0), index_address);
    auto* function = builder.GetInsertBlock()->getParent();
    auto* condition_block = llvm::BasicBlock::Create(context, "input.cond", function);
    auto* body_block = llvm::BasicBlock::Create(context, "input.body", function);
    auto* exit_block = llvm::BasicBlock::Create(context, "input.exit", function);
    builder.CreateBr(condition_block);

    builder.SetInsertPoint(condition_block);
    auto* index = builder.CreateLoad(builder.getInt32Ty(), index_address, "input.offset");
    auto* character = builder.CreateCall(getchar_function, {}, "input.char");
    auto* is_newline = builder.CreateICmpEQ(character, builder.getInt32('\n'));
    auto* is_eof = builder.CreateICmpEQ(character, builder.getInt32(-1));
    auto* is_full = builder.CreateICmpUGE(index, builder.getInt32(4095));
    auto* stop = builder.CreateOr(builder.CreateOr(is_newline, is_eof), is_full);
    builder.CreateCondBr(stop, exit_block, body_block);

    builder.SetInsertPoint(body_block);
    auto* element = builder.CreateInBoundsGEP(
        buffer_type, buffer, {builder.getInt32(0), index}, "input.element");
    builder.CreateStore(builder.CreateTrunc(character, builder.getInt8Ty()), element);
    auto* next = builder.CreateAdd(index, builder.getInt32(1), "input.next");
    builder.CreateStore(next, index_address);
    builder.CreateBr(condition_block);

    builder.SetInsertPoint(exit_block);
    auto* terminator = builder.CreateInBoundsGEP(
        buffer_type, buffer, {builder.getInt32(0), index}, "input.terminator");
    builder.CreateStore(builder.getInt8(0), terminator);
    return builder.CreateInBoundsGEP(
        buffer_type, buffer, {builder.getInt32(0), builder.getInt32(0)}, "input.line");
}

LValueInfo emit_lvalue(
    llvm::IRBuilder<>& builder,
    const ExpressionASTNode& expression,
    const SymbolTable& symbols,
    const FunctionTable& functions) {
    if (expression.kind == ExpressionASTNode::Kind::Identifier) {
        const auto found = symbols.find(expression.value);
        if (found == symbols.end()) throw std::runtime_error("use of undeclared variable '" + expression.value + "'");
        auto* value_type = found->second.value_type
            ? found->second.value_type : llvm_scalar_type(found->second.type, builder.getContext());
        return {found->second.address, found->second.type, found->second.mutability,
                value_type, found->second.struct_name};
    }
    if ((expression.kind == ExpressionASTNode::Kind::Dereference ||
         expression.kind == ExpressionASTNode::Kind::PointerUp) && expression.pointer) {
        const auto& operand = *expression.pointer->operand;
        if (operand.kind != ExpressionASTNode::Kind::Identifier) {
            throw std::runtime_error("pointer dereference currently requires a pointer variable");
        }
        const auto found = symbols.find(operand.value);
        if (found == symbols.end()) throw std::runtime_error("use of undeclared pointer '" + operand.value + "'");
        if (found->second.type != PeckType::Pointer || found->second.pointee_type == PeckType::Inferred) {
            throw std::runtime_error("cannot dereference an untyped pointer '" + operand.value + "'");
        }
        auto* pointer = expression_value(builder, operand, symbols, functions, PeckType::Pointer);
        auto* pointee_type = found->second.pointee_type == PeckType::Struct
            ? static_cast<llvm::Type*>(functions.structs.at(found->second.struct_name).type)
            : llvm_scalar_type(found->second.pointee_type, builder.getContext());
        return {pointer, found->second.pointee_type, found->second.pointee_mutability,
                pointee_type, found->second.struct_name};
    }
    if (expression.kind == ExpressionASTNode::Kind::MemberAccess && expression.member) {
        const auto& object = *expression.member->object;
        if (object.kind != ExpressionASTNode::Kind::Identifier) {
            throw std::runtime_error("struct member access requires a struct variable or pointer variable");
        }
        const auto variable = symbols.find(object.value);
        if (variable == symbols.end()) throw std::runtime_error("use of undeclared variable '" + object.value + "'");
        if (variable->second.struct_name.empty()) {
            throw std::runtime_error("member access requires a pointer to a declared struct");
        }
        const auto struct_info = functions.structs.find(variable->second.struct_name);
        if (struct_info == functions.structs.end()) throw std::runtime_error("unknown struct type");
        for (std::size_t index = 0; index < struct_info->second.fields.size(); ++index) {
            const auto& field = struct_info->second.fields[index];
            if (field.name != expression.member->member) continue;
            auto* object_pointer = expression_value(builder, object, symbols, functions, PeckType::Pointer);
            auto* field_address = builder.CreateStructGEP(
                struct_info->second.type, object_pointer, static_cast<unsigned>(index), field.name + ".addr");
            return {field_address, field.type, field.mutability,
                    llvm_scalar_type(field.type, builder.getContext()), {}};
        }
        throw std::runtime_error("struct '" + variable->second.struct_name +
                                 "' has no field '" + expression.member->member + "'");
    }
    throw std::runtime_error("expression is not assignable");
}

llvm::Value* expression_value(
    llvm::IRBuilder<>& builder,
    const ExpressionASTNode& expression,
    const SymbolTable& symbols,
    const FunctionTable& functions,
    PeckType expected_type) {
    if (expression.kind == ExpressionASTNode::Kind::Add && expression.left && expression.right) {
        auto* left = expression_value(builder, *expression.left, symbols, functions, expected_type);
        auto* right = expression_value(builder, *expression.right, symbols, functions, expected_type);
        if (expected_type == PeckType::Float || expected_type == PeckType::Double) {
            return builder.CreateFAdd(left, right, "add.value");
        }
        if (expected_type == PeckType::Integer || expected_type == PeckType::Byte) {
            return builder.CreateAdd(left, right, "add.value");
        }
        throw std::runtime_error("addition requires numeric operands");
    }
    if (expression.kind == ExpressionASTNode::Kind::AddressOf && expression.pointer) {
        return emit_lvalue(builder, *expression.pointer->operand, symbols, functions).address;
    }
    if ((expression.kind == ExpressionASTNode::Kind::Dereference ||
         expression.kind == ExpressionASTNode::Kind::PointerUp) && expression.pointer) {
        const auto target = emit_lvalue(builder, expression, symbols, functions);
        return builder.CreateLoad(target.llvm_type, target.address, "pointer.value");
    }
    if (expression.kind == ExpressionASTNode::Kind::PointerAt && expression.pointer) {
        return emit_lvalue(builder, *expression.pointer->operand, symbols, functions).address;
    }
    if (expression.kind == ExpressionASTNode::Kind::MemberAccess && expression.member) {
        const auto& object = *expression.member->object;
        if (object.kind == ExpressionASTNode::Kind::Identifier) {
            const auto enum_info = functions.enums.find(object.value);
            if (enum_info != functions.enums.end()) {
                for (std::size_t index = 0; index < enum_info->second.variants.size(); ++index) {
                    if (enum_info->second.variants[index] == expression.member->member) {
                        return llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(builder.getContext()), index);
                    }
                }
                throw std::runtime_error("enum '" + object.value + "' has no variant '" + expression.member->member + "'");
            }
        }
        const auto target = emit_lvalue(builder, expression, symbols, functions);
        return builder.CreateLoad(target.llvm_type, target.address, "member.value");
    }
    if (expression.kind == ExpressionASTNode::Kind::ReadInput ||
        expression.kind == ExpressionASTNode::Kind::ReadLine) {
        if (expected_type != PeckType::String) throw std::runtime_error("input() returns a string");
        return emit_input_line(builder);
    }
    if (expression.kind == ExpressionASTNode::Kind::Select) {
        const auto struct_info = functions.structs.find(expression.value);
        if (struct_info == functions.structs.end()) throw std::runtime_error("select() references unknown struct '" + expression.value + "'");
        auto* malloc_function = builder.GetInsertBlock()->getModule()->getFunction("malloc");
        const auto bytes = builder.GetInsertBlock()->getModule()->getDataLayout()
            .getTypeAllocSize(struct_info->second.type).getFixedValue();
        auto* storage = builder.CreateCall(malloc_function, {builder.getInt64(bytes)}, "select.heap");
        return storage;
    }
    if (expression.kind == ExpressionASTNode::Kind::Free) {
        throw std::runtime_error("free() is a statement and cannot be used as a value");
    }
    if (expression.kind == ExpressionASTNode::Kind::Call) {
        const auto found = functions.find(expression.value);
        if (found == functions.end()) throw std::runtime_error("call to undeclared function '" + expression.value + "'");
        const auto& info = found->second;
        if (!info.return_type) throw std::runtime_error("void function '" + expression.value + "' cannot be used as a value");
        if (*info.return_type != expected_type) {
            throw std::runtime_error("return type of function '" + expression.value + "' does not match expected type");
        }
        if (expression.arguments.size() != info.parameters.size()) {
            throw std::runtime_error("function '" + expression.value + "' called with the wrong number of arguments");
        }
        std::vector<llvm::Value*> arguments;
        arguments.reserve(expression.arguments.size());
        for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
            arguments.push_back(expression_value(
                builder, expression.arguments[index], symbols, functions, info.parameters[index].type));
        }
        return builder.CreateCall(info.value, arguments, expression.value + ".call");
    }
    if (expression.kind != ExpressionASTNode::Kind::Identifier) {
        return scalar_value(builder, expression, expected_type);
    }
    const auto found = symbols.find(expression.value);
    if (found == symbols.end()) throw std::runtime_error("use of undeclared variable '" + expression.value + "'");
    if (found->second.type != expected_type) {
        throw std::runtime_error("assignment type does not match variable '" + expression.value + "'");
    }
    if (expected_type == PeckType::Array || expected_type == PeckType::Data) {
        throw std::runtime_error("array/data values cannot be used as scalar expressions");
    }
    auto* type = expected_type == PeckType::Pointer && found->second.value_type
        ? found->second.value_type : llvm_scalar_type(expected_type, builder.getContext());
    return builder.CreateLoad(type, found->second.address, expression.value + ".value");
}

PeckType array_element_type(const VarDeclASTNode& declaration, const ArrayLiteralASTNode& array) {
    if (!declaration.type_suffix.empty()) return suffix_type(declaration.type_suffix);
    if (declaration.explicit_type && *declaration.explicit_type != PeckType::Array &&
        *declaration.explicit_type != PeckType::Inferred && *declaration.explicit_type != PeckType::Data) {
        return *declaration.explicit_type;
    }
    return array.elements.empty() ? PeckType::Integer : infer_type(array.elements.front());
}

void initialize_array(
    llvm::IRBuilder<>& builder,
    llvm::Value* address,
    llvm::ArrayType* array_type,
    const ArrayLiteralASTNode& array,
    PeckType element_type) {
    auto* zero = builder.getInt32(0);
    for (std::size_t index = 0; index < array.elements.size(); ++index) {
        const auto inferred = infer_type(array.elements[index]);
        const bool numeric_conversion = array.elements[index].kind == ExpressionASTNode::Kind::Number &&
            (element_type == PeckType::Float || element_type == PeckType::Double);
        if (inferred != element_type &&
            !(element_type == PeckType::Byte && inferred == PeckType::Integer) && !numeric_conversion) {
            throw std::runtime_error("array elements must have a consistent element type");
        }
        auto* element_address = builder.CreateInBoundsGEP(
            array_type, address, {zero, builder.getInt32(static_cast<std::uint32_t>(index))});
        builder.CreateStore(scalar_value(builder, array.elements[index], element_type), element_address);
    }
}

void emit_declaration(
    llvm::IRBuilder<>& builder,
    const VarDeclASTNode& declaration,
    SymbolTable& symbols,
    const FunctionTable& functions) {
    auto& context = builder.getContext();
    if (symbols.find(declaration.name) != symbols.end()) {
        throw std::runtime_error("variable '" + declaration.name + "' is already declared in this scope");
    }
    const auto resolved_type = resolve_type(declaration);
    llvm::Type* storage_type = nullptr;
    llvm::Value* initial_value = nullptr;

    if (resolved_type == PeckType::Data) {
        std::size_t byte_count = 0;
        if (const auto* text = std::get_if<ExpressionASTNode>(&declaration.initializer)) {
            if (text->kind != ExpressionASTNode::Kind::String) {
                throw std::runtime_error("data initializer must be a string or byte array");
            }
            byte_count = text->value.size() + 1;
        } else {
            byte_count = std::get<ArrayLiteralASTNode>(declaration.initializer).elements.size();
        }
        auto* byte_type = llvm::Type::getInt8Ty(context);
        auto* array_type = llvm::ArrayType::get(byte_type, byte_count);
        auto* address = builder.CreateAlloca(array_type, nullptr, declaration.name);
        if (const auto* text = std::get_if<ExpressionASTNode>(&declaration.initializer)) {
            const auto& bytes = text->value;
            for (std::size_t index = 0; index < byte_count; ++index) {
                const auto value = index < bytes.size() ? static_cast<unsigned char>(bytes[index]) : 0;
                auto* element_address = builder.CreateInBoundsGEP(
                    array_type, address, {builder.getInt32(0), builder.getInt32(static_cast<std::uint32_t>(index))});
                builder.CreateStore(builder.getInt8(value), element_address);
            }
        } else {
            const auto& array = std::get<ArrayLiteralASTNode>(declaration.initializer);
            initialize_array(builder, address, array_type, array, PeckType::Byte);
        }
        symbols.emplace(declaration.name, VariableSymbol{address, PeckType::Data, declaration.mutability});
        return;
    }

    if (resolved_type == PeckType::Array) {
        const auto& array = std::get<ArrayLiteralASTNode>(declaration.initializer);
        const auto element_type = array_element_type(declaration, array);
        auto* element_llvm_type = llvm_scalar_type(element_type, context);
        auto* array_type = llvm::ArrayType::get(element_llvm_type, array.elements.size());
        auto* address = builder.CreateAlloca(array_type, nullptr, declaration.name);
        initialize_array(builder, address, array_type, array, element_type);
        symbols.emplace(declaration.name, VariableSymbol{address, PeckType::Array, declaration.mutability});
        return;
    }

    if (resolved_type == PeckType::Enum) {
        const auto& initializer = std::get<ExpressionASTNode>(declaration.initializer);
        if (initializer.kind != ExpressionASTNode::Kind::MemberAccess || !initializer.member ||
            initializer.member->object->kind != ExpressionASTNode::Kind::Identifier) {
            throw std::runtime_error("enum variable requires a value such as EnumName.Variant");
        }
        const auto& enum_name = initializer.member->object->value;
        const auto enum_info = functions.enums.find(enum_name);
        if (enum_info == functions.enums.end()) throw std::runtime_error("unknown enum type '" + enum_name + "'");
        const auto tag = std::find(
            enum_info->second.variants.begin(), enum_info->second.variants.end(), initializer.member->member);
        if (tag == enum_info->second.variants.end()) {
            throw std::runtime_error("enum '" + enum_name + "' has no variant '" + initializer.member->member + "'");
        }
        auto* value_type = llvm::Type::getInt32Ty(context);
        auto* address = builder.CreateAlloca(value_type, nullptr, declaration.name);
        auto* value = llvm::ConstantInt::get(
            value_type, std::distance(enum_info->second.variants.begin(), tag));
        builder.CreateStore(value, address);
        symbols.emplace(declaration.name, VariableSymbol{
            address, PeckType::Enum, declaration.mutability, value_type,
            PeckType::Inferred, {}, MutabilityMode::Mutable, enum_name});
        return;
    }

    if (resolved_type == PeckType::Pointer) {
        const auto& initializer = std::get<ExpressionASTNode>(declaration.initializer);
        PeckType pointee_type = PeckType::Inferred;
        std::string struct_name;
        auto pointee_mutability = MutabilityMode::Mutable;
        if (initializer.kind == ExpressionASTNode::Kind::Select) {
            pointee_type = PeckType::Struct;
            struct_name = initializer.value;
        } else if ((initializer.kind == ExpressionASTNode::Kind::AddressOf ||
                initializer.kind == ExpressionASTNode::Kind::PointerAt) && initializer.pointer) {
            const auto target = emit_lvalue(builder, *initializer.pointer->operand, symbols, functions);
            pointee_type = target.type;
            struct_name = target.struct_name;
            pointee_mutability = target.mutability;
        } else if (initializer.kind == ExpressionASTNode::Kind::Identifier) {
            const auto found = symbols.find(initializer.value);
            if (found != symbols.end() && found->second.type == PeckType::Pointer) {
                pointee_type = found->second.pointee_type;
                struct_name = found->second.struct_name;
            }
        }
        auto* pointer_type = llvm::PointerType::getUnqual(context);
        auto* value = expression_value(builder, initializer, symbols, functions, PeckType::Pointer);
        auto* address = builder.CreateAlloca(pointer_type, nullptr, declaration.name);
        builder.CreateStore(value, address);
        symbols.emplace(declaration.name, VariableSymbol{
            address, PeckType::Pointer, declaration.mutability, pointer_type,
            pointee_type, struct_name, pointee_mutability});
        return;
    }

    storage_type = llvm_scalar_type(resolved_type, context);
    auto* address = builder.CreateAlloca(storage_type, nullptr, declaration.name);
    initial_value = expression_value(
        builder, std::get<ExpressionASTNode>(declaration.initializer), symbols, functions, resolved_type);
    builder.CreateStore(initial_value, address);
    symbols.emplace(declaration.name, VariableSymbol{
        address, resolved_type, declaration.mutability, storage_type});
}

llvm::Value* condition_value(
    llvm::IRBuilder<>& builder,
    const ExpressionASTNode& expression,
    const SymbolTable& symbols,
    const FunctionTable& functions,
    std::optional<PeckType> expected_type) {
    if (expression.kind == ExpressionASTNode::Kind::Identifier) {
        const auto found = symbols.find(expression.value);
        if (found == symbols.end()) throw std::runtime_error("use of undeclared variable '" + expression.value + "'");
        if (found->second.type == PeckType::Array || found->second.type == PeckType::Data ||
            found->second.type == PeckType::String) {
            throw std::runtime_error("if conditions require scalar numeric operands");
        }
        auto* type = llvm_scalar_type(found->second.type, builder.getContext());
        return builder.CreateLoad(type, found->second.address, expression.value + ".condition");
    }
    const auto type = expected_type.value_or(infer_type(expression));
    if (type == PeckType::String || type == PeckType::Data || type == PeckType::Array) {
        throw std::runtime_error("if conditions require scalar numeric operands");
    }
    return expression_value(builder, expression, symbols, functions, type);
}

llvm::Value* emit_condition(
    llvm::IRBuilder<>& builder,
    const ConditionASTNode& condition,
    const SymbolTable& symbols,
    const FunctionTable& functions) {
    std::optional<PeckType> common_type;
    if (condition.left.kind == ExpressionASTNode::Kind::Identifier) {
        const auto found = symbols.find(condition.left.value);
        if (found == symbols.end()) throw std::runtime_error("use of undeclared variable '" + condition.left.value + "'");
        common_type = found->second.type;
    } else if (condition.right.kind == ExpressionASTNode::Kind::Identifier) {
        const auto found = symbols.find(condition.right.value);
        if (found == symbols.end()) throw std::runtime_error("use of undeclared variable '" + condition.right.value + "'");
        common_type = found->second.type;
    } else {
        const auto left_type = infer_type(condition.left);
        const auto right_type = infer_type(condition.right);
        if (left_type != right_type) {
            if ((left_type == PeckType::Integer || left_type == PeckType::Float || left_type == PeckType::Double) &&
                (right_type == PeckType::Integer || right_type == PeckType::Float || right_type == PeckType::Double)) {
                common_type = (left_type == PeckType::Double || right_type == PeckType::Double)
                    ? PeckType::Double : PeckType::Float;
            } else {
                throw std::runtime_error("comparison operands must have compatible types");
            }
        } else {
            common_type = left_type;
        }
    }

    const auto type = *common_type;
    auto* left = condition_value(builder, condition.left, symbols, functions, type);
    auto* right = condition_value(builder, condition.right, symbols, functions, type);
    if (left->getType() != right->getType()) {
        throw std::runtime_error("comparison operands must have the same type");
    }

    if (type == PeckType::Float || type == PeckType::Double) {
        llvm::CmpInst::Predicate predicate;
        switch (condition.operation) {
        case ComparisonOperator::Equal: predicate = llvm::CmpInst::FCMP_OEQ; break;
        case ComparisonOperator::NotEqual: predicate = llvm::CmpInst::FCMP_ONE; break;
        case ComparisonOperator::Less: predicate = llvm::CmpInst::FCMP_OLT; break;
        case ComparisonOperator::LessEqual: predicate = llvm::CmpInst::FCMP_OLE; break;
        case ComparisonOperator::Greater: predicate = llvm::CmpInst::FCMP_OGT; break;
        case ComparisonOperator::GreaterEqual: predicate = llvm::CmpInst::FCMP_OGE; break;
        }
        return builder.CreateFCmp(predicate, left, right, "if.compare");
    }

    llvm::CmpInst::Predicate predicate;
    switch (condition.operation) {
    case ComparisonOperator::Equal: predicate = llvm::CmpInst::ICMP_EQ; break;
    case ComparisonOperator::NotEqual: predicate = llvm::CmpInst::ICMP_NE; break;
    case ComparisonOperator::Less: predicate = llvm::CmpInst::ICMP_SLT; break;
    case ComparisonOperator::LessEqual: predicate = llvm::CmpInst::ICMP_SLE; break;
    case ComparisonOperator::Greater: predicate = llvm::CmpInst::ICMP_SGT; break;
    case ComparisonOperator::GreaterEqual: predicate = llvm::CmpInst::ICMP_SGE; break;
    }
    return builder.CreateICmp(predicate, left, right, "if.compare");
}

void emit_statements(
    llvm::IRBuilder<>& builder,
    const std::vector<StatementASTNode>& statements,
    SymbolTable& symbols,
    llvm::Function* function,
    const FunctionTable& functions,
    const FunctionInfo& current_function);

void emit_if(
    llvm::IRBuilder<>& builder,
    const IfASTNode& statement,
    SymbolTable& symbols,
    llvm::Function* function,
    const FunctionTable& functions,
    const FunctionInfo& current_function) {
    auto* then_block = llvm::BasicBlock::Create(builder.getContext(), "if.then", function);
    auto* else_block = llvm::BasicBlock::Create(builder.getContext(), "if.else", function);
    auto* merge_block = llvm::BasicBlock::Create(builder.getContext(), "if.merge", function);
    builder.CreateCondBr(emit_condition(builder, statement.condition, symbols, functions), then_block, else_block);

    auto then_symbols = symbols;
    builder.SetInsertPoint(then_block);
    emit_statements(builder, statement.then_branch, then_symbols, function, functions, current_function);
    if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(merge_block);

    auto else_symbols = symbols;
    builder.SetInsertPoint(else_block);
    emit_statements(builder, statement.else_branch, else_symbols, function, functions, current_function);
    if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(merge_block);

    builder.SetInsertPoint(merge_block);
}

void emit_respon(
    llvm::IRBuilder<>& builder,
    const ResponASTNode& statement,
    SymbolTable& symbols,
    llvm::Function* function,
    const FunctionTable& functions,
    const FunctionInfo& current_function) {
    const auto variable = symbols.find(statement.variable);
    if (variable == symbols.end()) throw std::runtime_error("respon references undeclared variable '" + statement.variable + "'");
    if (variable->second.type != PeckType::Enum || variable->second.enum_name.empty()) {
        throw std::runtime_error("respon requires an enum variable");
    }
    const auto enum_info = functions.enums.find(variable->second.enum_name);
    if (enum_info == functions.enums.end()) throw std::runtime_error("unknown enum type '" + variable->second.enum_name + "'");

    auto* tag = builder.CreateLoad(llvm::Type::getInt32Ty(builder.getContext()),
                                   variable->second.address, statement.variable + ".tag");
    auto* merge_block = llvm::BasicBlock::Create(builder.getContext(), "respon.merge", function);
    auto* switch_instruction = builder.CreateSwitch(tag, merge_block, statement.cases.size());
    std::vector<bool> seen(enum_info->second.variants.size(), false);
    for (const auto& arm : statement.cases) {
        if (arm.enum_name != variable->second.enum_name) {
            throw std::runtime_error("respon pattern enum does not match variable '" + statement.variable + "'");
        }
        const auto variant = std::find(
            enum_info->second.variants.begin(), enum_info->second.variants.end(), arm.variant);
        if (variant == enum_info->second.variants.end()) {
            throw std::runtime_error("enum '" + arm.enum_name + "' has no variant '" + arm.variant + "'");
        }
        const auto index = static_cast<std::size_t>(std::distance(enum_info->second.variants.begin(), variant));
        if (seen[index]) throw std::runtime_error("duplicate respon case '" + arm.enum_name + "." + arm.variant + "'");
        seen[index] = true;
        auto* case_block = llvm::BasicBlock::Create(builder.getContext(), "respon.case", function);
        switch_instruction->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(builder.getContext()), index), case_block);
        builder.SetInsertPoint(case_block);
        auto case_symbols = symbols;
        emit_statements(builder, arm.body, case_symbols, function, functions, current_function);
        if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(merge_block);
    }
    builder.SetInsertPoint(merge_block);
}

llvm::Type* loop_counter_type(PeckType type, llvm::LLVMContext& context) {
    if (type == PeckType::Integer || type == PeckType::Byte || type == PeckType::Character) {
        return llvm_scalar_type(type, context);
    }
    throw std::runtime_error("for loop counter must be an integer type");
}

void emit_loop(
    llvm::IRBuilder<>& builder,
    const LoopASTNode& statement,
    SymbolTable& symbols,
    llvm::Function* function,
    const FunctionTable& functions,
    const FunctionInfo& current_function) {
    SymbolTable loop_symbols = symbols;
    if (statement.kind == LoopKind::For) {
        if (!statement.initializer) throw std::runtime_error("for loop requires a counter declaration");
        emit_declaration(builder, *statement.initializer, loop_symbols, functions);
    }

    auto* entry_block = llvm::BasicBlock::Create(builder.getContext(), "loop.entry", function);
    auto* body_block = llvm::BasicBlock::Create(builder.getContext(), "loop.body", function);
    auto* condition_block = llvm::BasicBlock::Create(builder.getContext(), "loop.cond", function);
    auto* exit_block = llvm::BasicBlock::Create(builder.getContext(), "loop.exit", function);
    builder.CreateBr(entry_block);

    builder.SetInsertPoint(entry_block);
    builder.CreateBr(body_block);

    builder.SetInsertPoint(body_block);
    emit_statements(builder, statement.body, loop_symbols, function, functions, current_function);
    if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(condition_block);

    builder.SetInsertPoint(condition_block);
    llvm::Value* repeat = nullptr;
    if (statement.kind == LoopKind::While) {
        repeat = statement.repeat_forever
            ? builder.getTrue()
            : emit_condition(builder, *statement.condition, loop_symbols, functions);
    } else {
        const auto& counter_name = statement.initializer->name;
        const auto counter = loop_symbols.find(counter_name);
        if (counter == loop_symbols.end()) throw std::runtime_error("for loop counter was not declared");
        const auto type = counter->second.type;
        auto* counter_type = loop_counter_type(type, builder.getContext());
        auto* current = builder.CreateLoad(counter_type, counter->second.address, counter_name + ".loop.value");
        auto* increment = type == PeckType::Integer
            ? static_cast<llvm::Value*>(builder.getInt32(1))
            : static_cast<llvm::Value*>(builder.getInt8(1));
        auto* next = builder.CreateAdd(current, increment, counter_name + ".loop.next");
        builder.CreateStore(next, counter->second.address);
        if (!statement.iteration_limit) throw std::runtime_error("for loop requires a cond-end iteration limit");
        auto* limit = expression_value(builder, *statement.iteration_limit, loop_symbols, functions, type);
        repeat = builder.CreateICmpSLT(next, limit, "loop.continue");
    }
    builder.CreateCondBr(repeat, body_block, exit_block);
    builder.SetInsertPoint(exit_block);
}

void emit_assignment(
    llvm::IRBuilder<>& builder,
    const AssignmentASTNode& assignment,
    SymbolTable& symbols,
    const FunctionTable& functions) {
    const auto target = emit_lvalue(builder, assignment.target, symbols, functions);
    if (target.mutability == MutabilityMode::Immutable || target.mutability == MutabilityMode::Constant) {
        throw std::runtime_error("cannot assign to immutable variable");
    }
    if (target.type == PeckType::Array || target.type == PeckType::Data || target.type == PeckType::Struct) {
        throw std::runtime_error("whole-array/data reassignment is not supported");
    }
    auto* value = expression_value(builder, assignment.value, symbols, functions, target.type);
    builder.CreateStore(value, target.address);
}

void emit_return(
    llvm::IRBuilder<>& builder,
    const ReturnASTNode& statement,
    const SymbolTable& symbols,
    const FunctionTable& functions,
    const FunctionInfo& current_function) {
    if (!current_function.return_type) {
        if (statement.value) throw std::runtime_error("void function cannot return a value");
        builder.CreateRetVoid();
        return;
    }
    if (!statement.value) throw std::runtime_error("non-void function must return a value");
    auto* value = expression_value(builder, *statement.value, symbols, functions, *current_function.return_type);
    builder.CreateRet(value);
}

bool is_builtin_meta_method(const std::string& name) {
    static const std::vector<std::string> names = {
        "update", "update.fn", "update.str", "update.other",
        "edit", "edit.fn", "edit.str", "edit.other", "not.edit", "ret.edit",
        "off", "on", "next", "off.next", "not.requir", "negative", "positive",
        "input.off",
    };
    return std::find(names.begin(), names.end(), name) != names.end();
}

void emit_meta_call(const ExpressionASTNode& call, const FunctionTable& functions) {
    if (is_builtin_meta_method(call.value)) return;
    const auto custom = functions.meta_methods.find(call.value);
    if (custom == functions.meta_methods.end()) {
        throw std::runtime_error("unknown function or meta-method '" + call.value + "'");
    }
    for (const auto& operation : custom->second.operations) {
        if (operation.kind != ExpressionASTNode::Kind::Call || !is_builtin_meta_method(operation.value)) {
            throw std::runtime_error("invalid operation in custom meta-method '" + call.value + "'");
        }
    }
}

void emit_statements(
    llvm::IRBuilder<>& builder,
    const std::vector<StatementASTNode>& statements,
    SymbolTable& symbols,
    llvm::Function* function,
    const FunctionTable& functions,
    const FunctionInfo& current_function) {
    for (const auto& statement : statements) {
        if (builder.GetInsertBlock()->getTerminator()) break;
        if (const auto* output = std::get_if<OutputCallASTNode>(&statement)) {
            auto* puts_function = function->getParent()->getFunction("puts");
            builder.CreateCall(puts_function, {builder.CreateGlobalStringPtr(output->text)});
        } else if (const auto* declaration = std::get_if<VarDeclASTNode>(&statement)) {
            emit_declaration(builder, *declaration, symbols, functions);
        } else if (const auto* assignment = std::get_if<AssignmentASTNode>(&statement)) {
            emit_assignment(builder, *assignment, symbols, functions);
        } else if (const auto* returned = std::get_if<ReturnASTNode>(&statement)) {
            emit_return(builder, *returned, symbols, functions, current_function);
        } else if (const auto* panic = std::get_if<PanicASTNode>(&statement)) {
            auto* puts_function = function->getParent()->getFunction("puts");
            builder.CreateCall(puts_function, {builder.CreateGlobalStringPtr(("panic: " + panic->reason).c_str())});
            builder.CreateCall(function->getParent()->getFunction("abort"), {});
            builder.CreateUnreachable();
        } else if (const auto* call = std::get_if<CallStatementASTNode>(&statement)) {
            if (call->call.kind == ExpressionASTNode::Kind::Free) {
                if (!call->call.left) throw std::runtime_error("free() requires a pointer argument");
                auto* pointer = expression_value(
                    builder, *call->call.left, symbols, functions, PeckType::Pointer);
                auto* free_function = function->getParent()->getFunction("free");
                builder.CreateCall(free_function, {pointer});
                continue;
            }
            if (call->call.kind == ExpressionASTNode::Kind::ReadInput ||
                call->call.kind == ExpressionASTNode::Kind::ReadLine) {
                (void)emit_input_line(builder);
                continue;
            }
            if (call->call.kind != ExpressionASTNode::Kind::Call) {
                throw std::runtime_error("expected a function call statement");
            }
            const auto found = functions.find(call->call.value);
            if (found == functions.end()) {
                emit_meta_call(call->call, functions);
                continue;
            }
            const auto& info = found->second;
            if (call->call.arguments.size() != info.parameters.size()) {
                throw std::runtime_error("function '" + call->call.value + "' called with the wrong number of arguments");
            }
            std::vector<llvm::Value*> arguments;
            for (std::size_t index = 0; index < call->call.arguments.size(); ++index) {
                arguments.push_back(expression_value(
                    builder, call->call.arguments[index], symbols, functions, info.parameters[index].type));
            }
            builder.CreateCall(info.value, arguments);
        } else if (const auto* conditional = std::get_if<IfASTNodePtr>(&statement)) {
            emit_if(builder, **conditional, symbols, function, functions, current_function);
        } else if (const auto* loop = std::get_if<LoopASTNodePtr>(&statement)) {
            emit_loop(builder, **loop, symbols, function, functions, current_function);
        } else if (const auto* respon = std::get_if<ResponASTNodePtr>(&statement)) {
            emit_respon(builder, **respon, symbols, function, functions, current_function);
        }
    }
}

llvm::Type* function_return_type(const FunctionASTNode& declaration, llvm::LLVMContext& context) {
    if (declaration.name == "main" && !declaration.return_type) return llvm::Type::getInt32Ty(context);
    if (!declaration.return_type) return llvm::Type::getVoidTy(context);
    if (declaration.name == "main" && *declaration.return_type != PeckType::Integer) {
        throw std::runtime_error("main return type must be !N");
    }
    return llvm_scalar_type(*declaration.return_type, context);
}

llvm::Function* declare_function(
    const FunctionASTNode& declaration,
    llvm::Module& module,
    llvm::LLVMContext& context) {
    std::vector<llvm::Type*> parameter_types;
    parameter_types.reserve(declaration.parameters.size());
    for (const auto& parameter : declaration.parameters) {
        parameter_types.push_back(llvm_scalar_type(parameter.type, context));
    }
    auto* signature = llvm::FunctionType::get(function_return_type(declaration, context), parameter_types, false);
    auto* function = llvm::Function::Create(signature, llvm::Function::ExternalLinkage, declaration.name, module);
    auto parameter = declaration.parameters.begin();
    for (auto& argument : function->args()) argument.setName(parameter++->name);
    return function;
}

void emit_function_body(
    const FunctionASTNode& declaration,
    FunctionInfo& info,
    const FunctionTable& functions) {
    auto* entry = llvm::BasicBlock::Create(info.value->getContext(), "entry", info.value);
    llvm::IRBuilder<> builder(entry);
    SymbolTable symbols;
    auto parameter = declaration.parameters.begin();
    for (auto& argument : info.value->args()) {
        const auto& descriptor = *parameter++;
        auto* address = builder.CreateAlloca(argument.getType(), nullptr, descriptor.name + ".addr");
        builder.CreateStore(&argument, address);
        symbols.emplace(descriptor.name, VariableSymbol{address, descriptor.type, descriptor.mutability});
    }

    emit_statements(builder, declaration.body, symbols, info.value, functions, info);
    if (builder.GetInsertBlock()->getTerminator()) return;
    if (info.is_entry_point) {
        builder.CreateRet(builder.getInt32(0));
    } else if (!info.return_type) {
        builder.CreateRetVoid();
    } else {
        throw std::runtime_error("function '" + declaration.name + "' can reach its end without returning a value");
    }
}

}

void emit_object(const Program& program, const std::filesystem::path& path) {
    if (llvm::InitializeNativeTarget()) throw std::runtime_error("could not initialize native LLVM target");
    if (llvm::InitializeNativeTargetAsmPrinter()) throw std::runtime_error("could not initialize native LLVM assembler printer");

    const auto triple = llvm::sys::getDefaultTargetTriple();
    std::string target_error;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, target_error);
    if (!target) throw std::runtime_error("could not find native LLVM target: " + target_error);

    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_));
    if (!machine) throw std::runtime_error("could not create native LLVM target machine");

    llvm::LLVMContext context;
    llvm::Module module("peck.module", context);
    module.setTargetTriple(triple);
    module.setDataLayout(machine->createDataLayout());

    llvm::IRBuilder<> builder(context);
    auto* puts_type = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy()}, false);
    llvm::Function::Create(puts_type, llvm::Function::ExternalLinkage, "puts", module);

    auto* malloc_type = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt64Ty()}, false);
    llvm::Function::Create(malloc_type, llvm::Function::ExternalLinkage, "malloc", module);
    auto* free_type = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
    llvm::Function::Create(free_type, llvm::Function::ExternalLinkage, "free", module);
    auto* abort_type = llvm::FunctionType::get(builder.getVoidTy(), false);
    llvm::Function::Create(abort_type, llvm::Function::ExternalLinkage, "abort", module);

    FunctionTable functions;
    for (const auto& declaration : program.enums) {
        if (functions.enums.find(declaration.name) != functions.enums.end()) {
            throw std::runtime_error("enum '" + declaration.name + "' is declared more than once");
        }
        functions.enums.emplace(declaration.name, EnumInfo{declaration.variants});
    }
    for (const auto& method : program.meta_methods) {
        if (!functions.meta_methods.emplace(method.name, method).second) {
            throw std::runtime_error("meta-method '" + method.name + "' is declared more than once");
        }
    }
    for (const auto& declaration : program.structs) {
        if (functions.structs.find(declaration.name) != functions.structs.end()) {
            throw std::runtime_error("struct '" + declaration.name + "' is declared more than once");
        }
        functions.structs.emplace(declaration.name, StructInfo{
            llvm::StructType::create(context, declaration.name), declaration.fields});
    }
    for (const auto& declaration : program.structs) {
        auto& info = functions.structs.at(declaration.name);
        std::vector<llvm::Type*> field_types;
        field_types.reserve(declaration.fields.size());
        for (const auto& field : declaration.fields) field_types.push_back(llvm_scalar_type(field.type, context));
        info.type->setBody(field_types, false);
    }
    for (const auto& declaration : program.functions) {
        if (functions.find(declaration.name) != functions.end()) {
            throw std::runtime_error("function '" + declaration.name + "' is declared more than once");
        }
        auto* function = declare_function(declaration, module, context);
        functions.emplace(declaration.name, FunctionInfo{
            function, declaration.return_type, declaration.parameters, declaration.name == "main"});
    }
    for (const auto& declaration : program.functions) {
        emit_function_body(declaration, functions.at(declaration.name), functions);
    }

    std::string verification_error;
    llvm::raw_string_ostream verification_stream(verification_error);
    if (llvm::verifyModule(module, &verification_stream)) {
        throw std::runtime_error("generated invalid LLVM module: " + verification_stream.str());
    }

    std::error_code file_error;
    llvm::raw_fd_ostream output(path.string(), file_error, llvm::sys::fs::OF_None);
    if (file_error) throw std::runtime_error("cannot write object file: " + file_error.message());

    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, output, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        throw std::runtime_error("native LLVM target cannot emit object files");
    }
    passes.run(module);
    output.flush();
    if (output.has_error()) throw std::runtime_error("failed while writing object file");
}

}