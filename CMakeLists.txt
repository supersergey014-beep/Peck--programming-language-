cmake_minimum_required(VERSION 3.20)
project(PeckCompiler LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")

add_executable(pk
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/codegen.cpp
)
target_include_directories(pk PRIVATE include src)
target_include_directories(pk SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(pk PRIVATE ${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBRARIES Core Support Target nativecodegen)
target_link_libraries(pk PRIVATE ${LLVM_LIBRARIES})

install(TARGETS pk RUNTIME DESTINATION bin)