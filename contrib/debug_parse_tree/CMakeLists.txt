# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Project name
project(debug_parse_tree)


# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_COMPILER g++)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb")

add_executable(debug_parse_tree main.cpp)
add_dependencies(debug_parse_tree cubridsa)
target_include_directories(debug_parse_tree PRIVATE ${EP_INCLUDES})
target_link_libraries(debug_parse_tree PRIVATE cubridsa)

install(TARGETS debug_parse_tree DESTINATION bin)

