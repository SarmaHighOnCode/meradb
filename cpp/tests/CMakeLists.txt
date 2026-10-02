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
  test_pyjson.cpp
  test_protocol.cpp
  test_engine_server_support.cpp
  test_server.cpp
  test_client.cpp
  test_client_server.cpp
  test_hardening.cpp
  test_stack_guard.cpp
  test_server_hardening.cpp
  test_server_control.cpp
  test_cli.cpp
  test_tokenizer_unicode.cpp
  test_pytext.cpp
  test_term_style.cpp
  test_repl_text.cpp
  test_repl_input.cpp
  test_repl.cpp
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
  set(_examples ${CMAKE_CURRENT_SOURCE_DIR}/../../examples)
  foreach(_script demo rdbms_lab_coverage)
    add_test(NAME cross_engine_full_${_script}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> ${_examples}/${_script}.mdb)
    add_test(NAME cross_engine_server_${_script}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> --via-server ${_examples}/${_script}.mdb)
  endforeach()
  foreach(_script demo_phase1 rdbms_lab_coverage_phase1)
    add_test(NAME cross_engine_server_${_script}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> --via-server
              ${CMAKE_CURRENT_SOURCE_DIR}/${_script}.mdb)
  endforeach()
  set_tests_properties(cross_engine_server_demo cross_engine_server_rdbms_lab_coverage
                       cross_engine_server_demo_phase1 cross_engine_server_rdbms_lab_coverage_phase1
                       PROPERTIES TIMEOUT 240)
  add_test(NAME cross_engine_missing_file
    COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> no_such_script.mdb)
  add_test(NAME cli_lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_lifecycle.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(cli_lifecycle PROPERTIES TIMEOUT 240)
  add_test(NAME cli_unicode
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_unicode.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(cli_unicode PROPERTIES TIMEOUT 240)
  add_test(NAME shell_lower_diff
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_lower_diff.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(shell_lower_diff PROPERTIES TIMEOUT 300)
  add_test(NAME cli_args_diff
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_args_diff.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(cli_args_diff PROPERTIES TIMEOUT 300)
  add_test(NAME interop_matrix
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/interop_check.py --cli $<TARGET_FILE:meradb_cli>)
  set_tests_properties(interop_matrix PROPERTIES TIMEOUT 900)
  add_test(NAME interchange
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/interchange_check.py --cli $<TARGET_FILE:meradb_cli>)
  add_test(NAME fuzz_triggers
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_triggers.py --cli $<TARGET_FILE:meradb_cli> --seeds 100)
  set_tests_properties(interchange fuzz_triggers PROPERTIES TIMEOUT 900)
  add_test(NAME shell_diff_local
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_diff.py --cli $<TARGET_FILE:meradb_cli> --mode local)
  add_test(NAME shell_diff_server
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_diff.py --cli $<TARGET_FILE:meradb_cli> --mode server)
  set_tests_properties(shell_diff_local PROPERTIES TIMEOUT 600)
  set_tests_properties(shell_diff_server PROPERTIES TIMEOUT 900)
  add_test(NAME shell_session
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_session.py --cli $<TARGET_FILE:meradb_cli>)
  set_tests_properties(shell_session PROPERTIES TIMEOUT 900)
else()
  message(STATUS "Python not found: cross-engine ctest checks skipped")
endif()
