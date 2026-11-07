cmake_minimum_required(VERSION 3.21)
project(couchsync VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COUCHSYNC_BUILD_TESTS "Build the Qt-free unit tests" ON)
option(COUCHSYNC_BUILD_APP "Build the Qt app (requires Qt 6 and libmpv)" ON)

# ---- Qt-free core (must not depend on Qt) -------------------------------------
add_subdirectory(src/sync)

if(COUCHSYNC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(NOT COUCHSYNC_BUILD_APP)
  return()
endif()

# ---- Qt ---------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 OpenGL)
qt_standard_project_setup(REQUIRES 6.5)

# ---- libmpv -----------------------------------------------------------------
set(MPV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv" CACHE PATH
    "libmpv dev files (Windows: output of tools/fetch-mpv.ps1)")

if(WIN32)
  set(_mpv_dll "${MPV_DIR}/libmpv-2.dll")
  set(_mpv_lib "${MPV_DIR}/mpv.lib")
  foreach(_f "${_mpv_dll}" "${_mpv_lib}" "${MPV_DIR}/include/mpv/client.h")
    if(NOT EXISTS "${_f}")
      message(FATAL_ERROR "Missing ${_f}. Run client/tools/fetch-mpv.ps1 from a VS x64 dev shell, or set MPV_DIR.")
    endif()
  endforeach()
  add_library(mpv::mpv SHARED IMPORTED)
  set_target_properties(mpv::mpv PROPERTIES
    IMPORTED_IMPLIB   "${_mpv_lib}"
    IMPORTED_LOCATION "${_mpv_dll}"
    INTERFACE_INCLUDE_DIRECTORIES "${MPV_DIR}/include")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MPV REQUIRED IMPORTED_TARGET GLOBAL mpv)
  add_library(mpv::mpv ALIAS PkgConfig::MPV)
endif()

# ---- app --------------------------------------------------------------------
qt_add_executable(couchsync src/main.cpp)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(couchsync
  URI CouchSync
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/FlatButton.qml
    qml/FlatSlider.qml
    qml/TrackPopup.qml
  SOURCES src/player/MpvItem.h src/player/MpvItem.cpp
)

target_link_libraries(couchsync PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::OpenGL mpv::mpv couchsync_sync)
# src/player is needed because the generated qmltyperegistrations.cpp includes "MpvItem.h" by basename.
target_include_directories(couchsync PRIVATE src src/player)

if(MSVC)
  target_compile_options(couchsync PRIVATE /utf-8 /W4)
endif()

# ---- Windows runtime deployment ----------------------------------------------
if(WIN32)
  option(COUCHSYNC_DEPLOY "Run windeployqt and copy libmpv next to the exe after build" ON)
  if(COUCHSYNC_DEPLOY)
    add_custom_command(TARGET couchsync POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:mpv::mpv> $<TARGET_FILE_DIR:couchsync>
      COMMAND Qt6::windeployqt
              --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml" --no-translations
              $<TARGET_FILE:couchsync>
      COMMENT "Deploying Qt runtime and libmpv"
      VERBATIM)
  endif()
endif()
