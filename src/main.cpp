#include "peck/compiler.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    bool emit_object = false;
};

void print_usage(std::ostream& stream) {
    stream << "Usage: pk <source.pk> [-o <output>] [-c|--emit-obj]\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (argument == "-c" || argument == "--emit-obj") {
            options.emit_object = true;
        } else if (argument == "-o") {
            if (++index >= argc) throw std::runtime_error("-o requires an output path");
            options.output = argv[index];
        } else if (!argument.empty() && argument[0] == '-') {
            throw std::runtime_error("unknown option: " + argument);
        } else if (options.input.empty()) {
            options.input = argument;
        } else {
            throw std::runtime_error("only one source file can be compiled at a time");
        }
    }
    if (options.input.empty()) throw std::runtime_error("no input source file provided");
    if (!options.input.has_extension() || options.input.extension() != ".pk") {
        throw std::runtime_error("input file must have a .pk extension");
    }
    if (options.output.empty()) {
        options.output = options.input;
        options.output.replace_extension(options.emit_object ? ".o" : "");
    }
    return options;
}

void link_executable(const std::filesystem::path& object, const std::filesystem::path& output) {
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("failed to start clang++ linker");
    if (child == 0) {
        execlp("clang++", "clang++", object.c_str(), "-o", output.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) throw std::runtime_error("failed while waiting for clang++ linker");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("clang++ failed to link the executable");
    }
}

}

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto program = peck::parse_file(options.input);
        if (options.emit_object) {
            peck::emit_object(program, options.output);
        } else {
            const auto object = std::filesystem::temp_directory_path() /
                ("peck-" + std::to_string(static_cast<long long>(getpid())) + ".o");
            try {
                peck::emit_object(program, object);
                link_executable(object, options.output);
                std::filesystem::remove(object);
            } catch (...) {
                std::filesystem::remove(object);
                throw;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "pk: " << error.what() << '\n';
        return 1;
    }
    return 0;
}