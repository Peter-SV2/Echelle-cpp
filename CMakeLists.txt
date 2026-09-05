# Echelle, in C++.
#
# ImGui and ImPlot are FETCHED at configure time, pinned to a tag, rather than
# vendored into the tree. The project's own reason for vendoring -- "a change
# reaches Echelle only when someone copies it across on purpose" -- is about
# pinning, and a pinned tag gives that without putting fifty thousand lines of
# someone else's source in the history.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# The self-check target builds and runs anywhere, with no GPU and no window:
#
#   cmake --build build --target check
cmake_minimum_required(VERSION 3.16)
project(Echelle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  # The app is a lot of small numeric loops; /fp:fast is measurably faster and
  # nothing here depends on strict reassociation order. The one place that
  # would -- the compensated sum in mean_of -- is written so it survives it.
  add_compile_options($<$<CONFIG:Release>:/O2>)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# --- the core: no window, no GPU, no platform ---------------------------------
add_library(echelle_core STATIC
  src/table.cpp src/fitstat.cpp src/spec.cpp src/gpexport.cpp src/ui_state.cpp)
target_include_directories(echelle_core PUBLIC src)

add_executable(selfcheck src/selfcheck.cpp)
target_link_libraries(selfcheck PRIVATE echelle_core)

add_executable(bench src/bench.cpp)
target_link_libraries(bench PRIVATE echelle_core)

enable_testing()
add_test(NAME selfcheck COMMAND selfcheck)
add_custom_target(check COMMAND selfcheck DEPENDS selfcheck)

# --- the GUI ------------------------------------------------------------------
# Windows only for now: the host loop is Win32 + D3D11. The core above is
# portable and is where everything that could be is kept.
if(WIN32)
  include(FetchContent)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
  FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(imgui implot)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp)
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${implot_SOURCE_DIR})
  # Third-party source is not ours to keep warning-clean.
  if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
  else()
    target_compile_options(imgui PRIVATE -w)
  endif()

  add_executable(Echelle WIN32 src/main.cpp src/ui.cpp
                 ${CMAKE_CURRENT_SOURCE_DIR}/echelle.rc)
  target_link_libraries(Echelle PRIVATE echelle_core imgui d3d11 dxgi d3dcompiler)
  set_target_properties(Echelle PROPERTIES OUTPUT_NAME "Echelle")
endif()
