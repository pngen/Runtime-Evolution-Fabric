# Runtime Evolution Fabric - test suite

set(REF_TEST_SUPPORT
  support/child_process.cpp
  support/fixtures.cpp
  support/child_process.hpp
  support/fixtures.hpp
  support/test_framework.hpp)

add_executable(ref_tests
  test_main.cpp
  test_core.cpp
  test_migration.cpp
  test_store_adversarial.cpp
  test_coordinator.cpp
  test_multiprocess.cpp
  test_property_race.cpp
  ${REF_TEST_SUPPORT})

target_include_directories(ref_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ref_tests PRIVATE SummonSoftwareLabs::RuntimeEvolutionFabric)
ref_apply_strict_flags(ref_tests)
# The multiprocess proofs need the real tool binaries.
target_compile_definitions(ref_tests PRIVATE REF_TOOL_DIR="$<TARGET_FILE_DIR:refcoordinator>")
add_dependencies(ref_tests refcoordinator refworker refcli)
set_target_properties(ref_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

add_test(NAME ref_tests COMMAND ref_tests)
