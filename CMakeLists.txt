# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(dualthrust VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL2 REQUIRED)

add_executable(dualthrust src/main.cpp)
target_link_libraries(dualthrust PRIVATE SDL2::SDL2)

# On some platforms the main target needs SDL2main
if(TARGET SDL2::SDL2main)
  target_link_libraries(dualthrust PRIVATE SDL2::SDL2main)
endif()

install(TARGETS dualthrust RUNTIME DESTINATION bin)
