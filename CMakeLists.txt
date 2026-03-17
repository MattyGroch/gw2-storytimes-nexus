cmake_minimum_required(VERSION 3.21)

project(gw2-storytimes-nexus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NEXUS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/_reference_nexus/src" CACHE PATH "Path to the Nexus SDK/source include root")
set(NEXUS_THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/_reference_nexus/src/thirdparty" CACHE PATH "Path to the Nexus third-party include root")

add_library(gw2storytimes SHARED
    src/main.cpp
    "${NEXUS_THIRDPARTY_DIR}/imgui/imgui.cpp"
    "${NEXUS_THIRDPARTY_DIR}/imgui/imgui_draw.cpp"
    "${NEXUS_THIRDPARTY_DIR}/imgui/imgui_tables.cpp"
    "${NEXUS_THIRDPARTY_DIR}/imgui/imgui_widgets.cpp"
)

target_include_directories(gw2storytimes PRIVATE
    "${NEXUS_INCLUDE_DIR}"
    "${NEXUS_THIRDPARTY_DIR}"
    "${NEXUS_THIRDPARTY_DIR}/imgui"
)

target_compile_definitions(gw2storytimes PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(gw2storytimes PRIVATE
    user32
    winhttp
)

set_target_properties(gw2storytimes PROPERTIES
    OUTPUT_NAME "gw2storytimes"
)
