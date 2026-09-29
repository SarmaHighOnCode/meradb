add_executable(meradb_tests
  test_sanity.cpp
  test_errors.cpp
  test_datatypes.cpp
  test_tokenizer.cpp
  test_ast.cpp
  test_parser_expressions.cpp
  test_parser_ddl.cpp
  test_parser_dml.cpp
  test_catalog.cpp
  test_storage.cpp
  test_table.cpp
  test_planner.cpp
  test_evaluator.cpp
  test_aggregates.cpp
  test_instance.cpp
  test_engine_ddl.cpp
  test_engine_dml.cpp
  test_engine_select.cpp
  test_engine_views_explain.cpp
  # test files are appended here by later tasks
)
target_link_libraries(meradb_tests PRIVATE meradb_core Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(meradb_tests)
