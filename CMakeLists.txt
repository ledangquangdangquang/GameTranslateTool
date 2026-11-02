cmake_minimum_required(VERSION 3.16)
project(unitytl C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(unitytl_core STATIC
  src/bytes.cpp src/json.cpp src/lz4.cpp src/bzip2.cpp src/bundle.cpp src/serialized.cpp
  src/typetree.cpp src/csv.cpp src/formats.cpp src/catalog.cpp src/translate.cpp src/project.cpp
  src/downloader.cpp src/gamemaker.cpp src/godot.cpp src/ttf.cpp src/renpy.cpp src/unity_font.cpp
  src/probe.cpp src/unreal.cpp
  third_party/zstd/zstddeclib.c)
target_include_directories(unitytl_core PRIVATE third_party/zstd)
target_link_libraries(unitytl_core PUBLIC winhttp bcrypt dxgi dxguid windowscodecs gdi32 ole32 uuid z)

set(IMGUI third_party/imgui)
add_library(imgui STATIC
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/misc/cpp/imgui_stdlib.cpp
  ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)

# GUI app (no console window)
add_executable(unitytl WIN32 src/gui.cpp)
target_link_libraries(unitytl PRIVATE unitytl_core imgui d3d11 d3dcompiler dxgi dwmapi ole32 shell32 uuid)

# command line tool
add_executable(unitytl-cli src/cli.cpp)
target_link_libraries(unitytl-cli PRIVATE unitytl_core)

if(MINGW)
  # single portable .exe: no libstdc++/winpthread DLLs needed next to it
  foreach(t unitytl unitytl-cli)
    target_link_options(${t} PRIVATE -static -municode)
  endforeach()
endif()
