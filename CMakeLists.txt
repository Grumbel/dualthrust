# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

# Source of truth: top-level VERSION (e.g. "0.1.0-dev"), or -DPROJECT_VERSION_FULL=... from packaging
# (Nix appends .<revCount>+g<rev> for -dev builds).
if(NOT DEFINED PROJECT_VERSION_FULL)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VERSION_FULL LIMIT_COUNT 1)
endif()
# project() needs a numeric version; strip -dev / +git suffixes.
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" _project_version_cmake "${PROJECT_VERSION_FULL}")

project(dualthrust VERSION ${_project_version_cmake} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

if(EMSCRIPTEN)
  # WebAssembly. SDL2 is either a static build under SDL2_ROOT (headers and libSDL2.a, as nix/wasm.nix
  # makes it, offline) or Emscripten's own port, which it downloads on first use. Emscripten's
  # toolchain limits find_*() to its sysroot, so SDL2_ROOT is used directly.
  if(SDL2_ROOT)
    if(NOT EXISTS "${SDL2_ROOT}/lib/libSDL2.a" OR NOT EXISTS "${SDL2_ROOT}/include/SDL2/SDL.h")
      message(FATAL_ERROR "SDL2_ROOT=${SDL2_ROOT} has no lib/libSDL2.a or include/SDL2/SDL.h")
    endif()
    add_library(SDL2::SDL2 STATIC IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
      IMPORTED_LOCATION "${SDL2_ROOT}/lib/libSDL2.a"
      INTERFACE_INCLUDE_DIRECTORIES "${SDL2_ROOT}/include/SDL2")
  else()
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
      INTERFACE_COMPILE_OPTIONS "-sUSE_SDL=2" INTERFACE_LINK_OPTIONS "-sUSE_SDL=2")
  endif()
else()
  # CMake config package when available; pkg-config otherwise (e.g. the ArkOS sysroot).
  find_package(SDL2 QUIET CONFIG)
  if(NOT TARGET SDL2::SDL2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2)
    add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
  endif()
endif()

add_executable(dualthrust
  src/main.cpp
  src/audio.cpp
  src/backend_gles2.cpp
  src/backend_sdl.cpp
  src/cave.cpp
  src/config.cpp
  src/render.cpp
  src/systems.cpp
)
target_compile_options(dualthrust PRIVATE -Wall -Wextra)
target_compile_definitions(dualthrust PRIVATE DUALTHRUST_VERSION="${PROJECT_VERSION_FULL}")

# Optional ABI shims when a newer compiler links against an older libstdc++ (R36S / ArkOS).
if(DUALTHRUST_CXXABI_SHIM)
  target_sources(dualthrust PRIVATE "${DUALTHRUST_CXXABI_SHIM}")
endif()
target_link_libraries(dualthrust PRIVATE SDL2::SDL2)

if(TARGET SDL2::SDL2main)
  target_link_libraries(dualthrust PRIVATE SDL2::SDL2main)
endif()

if(EMSCRIPTEN)
  # The web page: dualthrust.html (also as index.html) with its .js and .wasm, installed at the top of
  # the prefix, ready to serve as a static site. The shell carries the version and source revision
  # (pass -DDUALTHRUST_GIT_REV=..., as Nix does).
  if(NOT DEFINED DUALTHRUST_GIT_REV)
    execute_process(COMMAND git rev-parse --short HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE DUALTHRUST_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _git_status ERROR_QUIET)
    if(NOT _git_status EQUAL 0 OR DUALTHRUST_GIT_REV STREQUAL "")
      set(DUALTHRUST_GIT_REV "unknown")
    endif()
  endif()
  set(DUALTHRUST_SOURCE_URL "https://github.com/Grumbel/dualthrust")
  configure_file(mk/wasm/shell.html ${CMAKE_CURRENT_BINARY_DIR}/shell.html @ONLY)

  set_target_properties(dualthrust PROPERTIES SUFFIX ".html")
  target_link_options(dualthrust PRIVATE
    --shell-file ${CMAKE_CURRENT_BINARY_DIR}/shell.html
    -sALLOW_MEMORY_GROWTH=1
    # Emscripten 6 makes growable memory a resizable ArrayBuffer, which TextDecoder rejects;
    # grow the classic way.
    -sGROWABLE_ARRAYBUFFERS=0
    # The page keeps the settings directory in IndexedDB (IDBFS).
    -sFORCE_FILESYSTEM=1
    -lidbfs.js
    -sEXPORTED_RUNTIME_METHODS=FS,addRunDependency,removeRunDependency
    -sEXPORTED_FUNCTIONS=_main,_dualthrust_pause
    -sENVIRONMENT=web)
  set_property(TARGET dualthrust APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mk/wasm/shell.html)

  set(_web ${CMAKE_CURRENT_BINARY_DIR}/dualthrust)
  install(FILES ${_web}.html ${_web}.js ${_web}.wasm DESTINATION .)
  install(FILES ${_web}.html DESTINATION . RENAME index.html)
  return()  # no desktop integration for the web build
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

# Man page carries the full version
configure_file(data/man/dualthrust.1.in ${CMAKE_CURRENT_BINARY_DIR}/dualthrust.1 @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dualthrust.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
