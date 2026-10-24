cmake_minimum_required(VERSION 3.21)
project(menuluaruncontrol LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "menuluaruncontrol requires MSVC on Windows (Win32 or x64).")
endif()

option(MENULUARUNCONTROL_STDOUT "Build the _stdout_ DLL with an async console" OFF)

include(FetchContent)
# Lua headers are fetched during configuration, not when loading the DLL.
FetchContent_Declare(garrysmod_common
  GIT_REPOSITORY https://github.com/danielga/garrysmod_common.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(garrysmod_common)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(TARGET_BITS 32)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(TARGET_BITS 64)
else()
  message(FATAL_ERROR "Only Win32 and x64 are supported")
endif()

add_library(menuluaruncontrol SHARED src/menuluaruncontrol.cpp)
target_include_directories(menuluaruncontrol PRIVATE "${garrysmod_common_SOURCE_DIR}/include")
target_compile_features(menuluaruncontrol PRIVATE cxx_std_17)
target_compile_definitions(menuluaruncontrol PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(menuluaruncontrol PRIVATE /W4 /EHsc /permissive-)
set_property(TARGET menuluaruncontrol PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MENULUARUNCONTROL_STDOUT)
  target_sources(menuluaruncontrol PRIVATE src/stdout_console.cpp)
  target_compile_definitions(menuluaruncontrol PRIVATE MENULUARUNCONTROL_STDOUT=1)
  set(DLL_NAME "menuluaruncontrol_stdout_win${TARGET_BITS}")
else()
  set(DLL_NAME "menuluaruncontrol_win${TARGET_BITS}")
endif()

set_target_properties(menuluaruncontrol PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${DLL_NAME}"
)
# No HTTP code, log files or third-party detouring library.
target_link_libraries(menuluaruncontrol PRIVATE user32)
