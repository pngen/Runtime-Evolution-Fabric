# Runtime Evolution Fabric - benchmarks

add_executable(ref_benchmarks bench_core.cpp)
target_link_libraries(ref_benchmarks PRIVATE SummonSoftwareLabs::RuntimeEvolutionFabric)
ref_apply_strict_flags(ref_benchmarks)
set_target_properties(ref_benchmarks PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
