cmake_minimum_required(VERSION 3.20)

# An Intel CMake running under Rosetta on Apple silicon would otherwise build
# Intel binaries that also run translated.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  execute_process(COMMAND sysctl -in sysctl.proc_translated
                  OUTPUT_VARIABLE FLZ_TRANSLATED OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(FLZ_TRANSLATED STREQUAL "1")
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architectures" FORCE)
  endif()
endif()

project(flz VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(flz src/encoder.cpp src/decoder.cpp src/container.cpp)
target_include_directories(flz PUBLIC include)
target_link_libraries(flz PRIVATE Threads::Threads)
if(MSVC)
  target_compile_options(flz PRIVATE /W4)
else()
  target_compile_options(flz PRIVATE -Wall -Wextra -Wconversion -Wshadow)
endif()

add_executable(flz_cli tools/flz_cli.cpp)
set_target_properties(flz_cli PROPERTIES OUTPUT_NAME flz)
target_link_libraries(flz_cli PRIVATE flz)

include(CTest)
if(BUILD_TESTING)
  add_executable(flz_tests tests/flz_tests.cpp)
  target_link_libraries(flz_tests PRIVATE flz)
  add_test(NAME flz_tests COMMAND flz_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
endif()
