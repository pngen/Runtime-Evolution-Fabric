# Runtime Evolution Fabric - examples
#
# Each example is a standalone program that uses only the public API.

set(REF_EXAMPLES
  01_basic_upgrade
  02_mixed_version_protocol
  03_schema_migration
  04_feature_gate
  05_rollback
  06_irreversible_barrier
  07_worker_fencing
  08_coordinator_restart
  09_installed_consumer)

foreach(example ${REF_EXAMPLES})
  add_executable(ref_example_${example} ${example}.cpp)
  target_link_libraries(ref_example_${example} PRIVATE SummonSoftwareLabs::RuntimeEvolutionFabric)
  ref_apply_strict_flags(ref_example_${example})
  set_target_properties(ref_example_${example} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
endforeach()
