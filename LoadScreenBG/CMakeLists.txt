cmake_minimum_required(VERSION 4.2)

if(NOT DEFINED VCPKG_ROOT AND DEFINED ENV{VCPKG_ROOT})
  set(VCPKG_ROOT "$ENV{VCPKG_ROOT}")
endif()
if(NOT DEFINED VCPKG_ROOT OR VCPKG_ROOT STREQUAL "")
  message(FATAL_ERROR "VCPKG_ROOT is required")
endif()
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

project(LoadScreenBG VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed")
endif()

set(COMMON_LIB_F4VR_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../extern/CommonLibF4VR" CACHE PATH "CommonLibF4VR checkout")

set(F4SE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_NG OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_F4 OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_VR ON CACHE BOOL "" FORCE)
add_subdirectory("${COMMON_LIB_F4VR_PATH}" "CommonLibF4" EXCLUDE_FROM_ALL)

find_package(spdlog CONFIG REQUIRED)

if(DEFINED ENV{GITHUB_SHA})
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 LOADSCREENBG_BUILD)
else()
  set(LOADSCREENBG_BUILD "local")
endif()

add_library(LoadScreenBG SHARED
  src/LoadScreenBG.cpp
)
target_compile_features(LoadScreenBG PRIVATE cxx_std_23)
target_compile_definitions(LoadScreenBG PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  NOMMNOSOUND
  LOADSCREENBG_BUILD="${LOADSCREENBG_BUILD}"
)
target_link_libraries(LoadScreenBG PRIVATE
  CommonLibF4::CommonLibF4
  spdlog::spdlog
)
target_compile_options(LoadScreenBG PRIVATE
  /sdl
  /utf-8
  /permissive-
  /Zc:__cplusplus
  /Zc:preprocessor
  /W4
  "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC->"
)
target_link_options(LoadScreenBG PRIVATE
  "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)
set_target_properties(LoadScreenBG PROPERTIES PREFIX "")
