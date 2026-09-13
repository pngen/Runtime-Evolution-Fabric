cmake_minimum_required(VERSION 3.20)

project(RuntimeEvolutionFabricConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent downstream consumer of the installed Runtime Evolution Fabric package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The consumer only knows about the installed package.
find_package(RuntimeEvolutionFabric CONFIG REQUIRED)

add_executable(ref_consumer main.cpp)
target_link_libraries(ref_consumer PRIVATE SummonSoftwareLabs::RuntimeEvolutionFabric)

if(MSVC)
  target_compile_options(ref_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(ref_consumer PRIVATE -Wall -Wextra -Werror)
endif()

message(STATUS "RuntimeEvolutionFabric ${RuntimeEvolutionFabric_VERSION} found at ${RuntimeEvolutionFabric_DIR}")
