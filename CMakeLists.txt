cmake_minimum_required(VERSION 3.25)
project(ShadowAntFarm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Deterministic floating point: no fast-math, no FMA contraction differences between call sites.
set(ANTFARM_FP_FLAGS -fno-fast-math -ffp-contract=off)

option(ANTFARM_BUILD_EXTENSION "Build the Godot GDExtension (needs third_party/godot-cpp)" ON)

add_library(antfarm_core STATIC
  core/src/config.cpp
  core/src/world.cpp
  core/src/fields.cpp
  core/src/sim.cpp)
target_include_directories(antfarm_core PUBLIC core/include)
target_compile_options(antfarm_core PRIVATE -Wall -Wextra ${ANTFARM_FP_FLAGS})

add_executable(antfarm_tests tests/core_tests.cpp)
target_link_libraries(antfarm_tests PRIVATE antfarm_core)
target_compile_options(antfarm_tests PRIVATE ${ANTFARM_FP_FLAGS})

add_executable(antfarm_sim tools/antfarm_sim.cpp)
target_link_libraries(antfarm_sim PRIVATE antfarm_core)
target_compile_options(antfarm_sim PRIVATE ${ANTFARM_FP_FLAGS})

enable_testing()
add_test(NAME core COMMAND antfarm_tests)

if(ANTFARM_BUILD_EXTENSION)
  set(GODOTCPP_API_VERSION "4.7" CACHE STRING "" FORCE)
  set(GODOTCPP_TARGET "template_release" CACHE STRING "" FORCE)
  add_subdirectory(third_party/godot-cpp EXCLUDE_FROM_ALL)
  add_library(antfarm_gdext SHARED
    extension/src/register_types.cpp
    extension/src/ant_farm_sim.cpp
    extension/src/ant_audio.cpp)
  target_link_libraries(antfarm_gdext PRIVATE antfarm_core godot-cpp)
  target_compile_options(antfarm_gdext PRIVATE ${ANTFARM_FP_FLAGS})
  set_target_properties(antfarm_gdext PROPERTIES
    OUTPUT_NAME "antfarm"
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/game/bin")
endif()
