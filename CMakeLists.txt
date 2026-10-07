# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(dualthrust VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(SDL2 REQUIRED)

add_executable(dualthrust src/main.cpp)
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
