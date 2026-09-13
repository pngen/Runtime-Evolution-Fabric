cmake_minimum_required(VERSION 3.20)

project(RuntimeEvolutionFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for governing live evolution of distributed runtime components"
  HOMEPAGE_URL "https://github.com/pngen/Runtime-Evolution-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(REF_BUILD_TESTS      "Build the Runtime Evolution Fabric test suite" ON)
option(REF_BUILD_EXAMPLES   "Build the Runtime Evolution Fabric examples" ON)
option(REF_BUILD_BENCHMARKS "Build the Runtime Evolution Fabric benchmarks" ON)
option(REF_BUILD_TOOLS      "Build the coordinator, worker and CLI tools" ON)
option(REF_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(REF_ENABLE_ASAN      "Build first-party targets with AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Strict first-party warning contract
# ---------------------------------------------------------------------------
# Applied directly to each first-party target instead of through an interface
# library so that the installed export set only references real dependencies.
function(ref_apply_strict_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    if(REF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(REF_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
    endif()
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Wcast-qual -Wdouble-promotion)
    if(REF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(REF_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(REF_PUBLIC_HEADERS
  include/ref/ids.hpp
  include/ref/support.hpp
  include/ref/component.hpp
  include/ref/compat.hpp
  include/ref/protocol.hpp
  include/ref/schema.hpp
  include/ref/migration.hpp
  include/ref/plan.hpp
  include/ref/authority.hpp
  include/ref/store.hpp
  include/ref/wire.hpp
  include/ref/coordinator.hpp
  include/ref/client.hpp
  include/ref/worker.hpp
  include/ref/ref.hpp)

set(REF_SOURCES
  src/ids.cpp
  src/support.cpp
  src/component.cpp
  src/compat.cpp
  src/protocol.cpp
  src/schema.cpp
  src/migration.cpp
  src/plan.cpp
  src/authority.cpp
  src/store.cpp
  src/wire.cpp
  src/coordinator.cpp
  src/coordinator_internal.cpp
  src/coordinator_commands.cpp
  src/coordinator_actions.cpp
  src/coordinator_net.cpp
  src/coordinator_query.cpp
  src/client.cpp
  src/worker.cpp)

add_library(RuntimeEvolutionFabric STATIC ${REF_SOURCES} ${REF_PUBLIC_HEADERS})
add_library(SummonSoftwareLabs::RuntimeEvolutionFabric ALIAS RuntimeEvolutionFabric)

target_include_directories(RuntimeEvolutionFabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(RuntimeEvolutionFabric PUBLIC Threads::Threads)

set_target_properties(RuntimeEvolutionFabric PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  # Winsock is required by the framed TCP transport; it is a system library and
  # is safe to export verbatim.
  target_link_libraries(RuntimeEvolutionFabric PUBLIC ws2_32)
endif()

ref_apply_strict_flags(RuntimeEvolutionFabric)

# ---------------------------------------------------------------------------
# Tools, tests, examples, benchmarks
# ---------------------------------------------------------------------------
if(REF_BUILD_TOOLS)
  add_subdirectory(apps)
endif()
if(REF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(REF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(REF_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install / packaging
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS RuntimeEvolutionFabric
  EXPORT RuntimeEvolutionFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT RuntimeEvolutionFabricTargets
  FILE RuntimeEvolutionFabricTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RuntimeEvolutionFabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RuntimeEvolutionFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/RuntimeEvolutionFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RuntimeEvolutionFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/RuntimeEvolutionFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/RuntimeEvolutionFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/RuntimeEvolutionFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RuntimeEvolutionFabric)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/RuntimeEvolutionFabric)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/RuntimeEvolutionFabric)