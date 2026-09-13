# Runtime Evolution Fabric - tools

set(REF_APP_HEADERS cli_arguments.hpp)

add_executable(refcoordinator refcoordinator.cpp ${REF_APP_HEADERS})
add_executable(refworker refworker.cpp ${REF_APP_HEADERS})
add_executable(refcli refcli.cpp ${REF_APP_HEADERS})

foreach(tool refcoordinator refworker refcli)
  target_link_libraries(${tool} PRIVATE SummonSoftwareLabs::RuntimeEvolutionFabric)
  ref_apply_strict_flags(${tool})
  set_target_properties(${tool} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
endforeach()

install(TARGETS refcoordinator refworker refcli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
