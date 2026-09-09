cmake_minimum_required(VERSION 3.20)

project(unit_lcd_plus_ui_simulator LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

# LVGL 9.3.0 release commit. Pinning the commit keeps the target and simulator API stable.
FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG c033a98afddd65aaafeebea625382a94020fe4a7
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(lvgl)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# smooth_ui_toolkit (vendored src/core subset, see lib/smooth_ui_toolkit/README.md).
# Kept as its own target so the app's -Werror does not apply to third-party code.
file(GLOB_RECURSE SMOOTH_UI_TOOLKIT_CORE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/../lib/smooth_ui_toolkit/src/core/*.cpp"
)
add_library(smooth_ui_toolkit_core STATIC ${SMOOTH_UI_TOOLKIT_CORE_SOURCES})
target_include_directories(smooth_ui_toolkit_core SYSTEM PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../lib/smooth_ui_toolkit/src"
)

add_executable(unit-lcd-ui-sim
  main.cpp
  ../ui/unit_lcd_ui.cpp
)

target_include_directories(unit-lcd-ui-sim PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../ui"
)
target_link_libraries(unit-lcd-ui-sim PRIVATE
  lvgl::lvgl
  smooth_ui_toolkit_core
  PkgConfig::SDL2
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(unit-lcd-ui-sim PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
  )
endif()
