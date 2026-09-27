add_executable(meradb_tests
  test_sanity.cpp
  test_errors.cpp
  test_datatypes.cpp
  # test files are appended here by later tasks (already listed above)
)
target_link_libraries(meradb_tests PRIVATE meradb_core Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(meradb_tests)
