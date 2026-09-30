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
  test_engine_users.cpp
  test_engine_triggers.cpp
  test_engine_procedures.cpp
  test_substitute.cpp
  test_engine_views_explain.cpp
  test_cli_output.cpp
  test_sys_compat.cpp
  test_parser_phase2.cpp
  test_crypto.cpp
  test_users.cpp
  test_net_compat.cpp
  # test files are appended here by later tasks
)
target_link_libraries(meradb_tests PRIVATE meradb_core Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(meradb_tests)

# Cross-engine diffs need Python (and the Python reference engine); skip cleanly if absent.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(_diff ${CMAKE_CURRENT_SOURCE_DIR}/cross_engine_diff.py)
  foreach(_script demo_phase1 rdbms_lab_coverage_phase1)
    add_test(NAME cross_engine_${_script}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli>
              ${CMAKE_CURRENT_SOURCE_DIR}/${_script}.mdb)
  endforeach()
  # The shipped examples exactly as checked out (CRLF on Windows): exercises universal-newline reading.
  foreach(_example demo rdbms_lab_coverage)
    add_test(NAME cross_engine_example_${_example}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli>
              ${CMAKE_CURRENT_SOURCE_DIR}/../../examples/${_example}.mdb)
  endforeach()
  add_test(NAME cross_engine_missing_file
    COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> no_such_script.mdb)
else()
  message(STATUS "Python not found: cross-engine ctest checks skipped")
endif()
