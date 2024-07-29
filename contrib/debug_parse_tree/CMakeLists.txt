# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Project name
project(debug_parse_tree)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add the executable
add_executable(debug_parse_tree main.cpp)
add_dependencies(debug_parse_tree cubridsa cubridcs)
target_link_libraries(debug_parse_tree PRIVATE cubridsa cubridcs)
target_include_directories(debug_parse_tree PRIVATE ${EP_INCLUDES})

install(TARGETS debug_parse_tree DESTINATION bin)

