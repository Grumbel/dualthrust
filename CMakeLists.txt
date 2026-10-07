# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(dualthrust VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# CMake config package when available; pkg-config otherwise (e.g. the ArkOS sysroot).
find_package(SDL2 QUIET CONFIG)
if(NOT TARGET SDL2::SDL2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2)
  add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
endif()

add_executable(dualthrust
  src/main.cpp
  src/audio.cpp
  src/cave.cpp
  src/config.cpp
  src/render.cpp
  src/systems.cpp
)
target_compile_options(dualthrust PRIVATE -Wall -Wextra)

# Optional ABI shims when a newer compiler links against an older libstdc++ (R36S / ArkOS).
if(DUALTHRUST_CXXABI_SHIM)
  target_sources(dualthrust PRIVATE "${DUALTHRUST_CXXABI_SHIM}")
endif()
target_link_libraries(dualthrust PRIVATE SDL2::SDL2)

if(TARGET SDL2::SDL2main)
  target_link_libraries(dualthrust PRIVATE SDL2::SDL2main)
endif()

install(TARGETS dualthrust
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES data/dualthrust.desktop
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)

install(FILES data/icons/hicolor/scalable/apps/dualthrust.svg
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
)

install(FILES data/icons/hicolor/48x48/apps/dualthrust.png
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps
)

install(FILES data/icons/hicolor/256x256/apps/dualthrust.png
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
)

install(FILES data/metainfo/dualthrust.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
)

install(FILES data/man/dualthrust.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
