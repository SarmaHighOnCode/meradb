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
  test_tokenizer_space.cpp
  test_tokenizer_unicode.cpp
  test_pytext.cpp
  test_term_style.cpp
  test_repl_text.cpp
  test_repl_input.cpp
  test_repl.cpp
  test_ws_text.cpp
  test_backend_closer.cpp
  test_highlight.cpp
  # test files are appended here by later tasks
)
target_link_libraries(meradb_tests PRIVATE meradb_core Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(meradb_tests)

if(TARGET meradb_workbench)
  add_executable(meradb_wb_tests
    test_wb_sanity.cpp
    test_wb_text.cpp
    test_wb_editor.cpp
    test_wb_tree.cpp
    test_wb_worker.cpp
    test_wb_session.cpp
    test_wb_help.cpp
    test_wb_panels.cpp
    test_wb_window.cpp
    test_wb_form.cpp
    test_wb_input.cpp
    test_wb_dialogs.cpp
    test_wb_bridge.cpp
    test_wb_e2e.cpp
  )
  target_link_libraries(meradb_wb_tests PRIVATE meradb_workbench meradb_core Catch2::Catch2WithMain)
  target_include_directories(meradb_wb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ftxui_patch)
  target_compile_definitions(meradb_wb_tests PRIVATE MERADB_LANGUAGE_MD="${CMAKE_CURRENT_SOURCE_DIR}/../../docs/LANGUAGE.md")
  catch_discover_tests(meradb_wb_tests)

  # A developer tool, not a test: shows the raw bytes your terminal sends for each key (see Task 12 / 14).
  add_executable(wb_keyprobe tools/wb_keyprobe.cpp)
  target_link_libraries(wb_keyprobe PRIVATE meradb_workbench meradb_core)

  # Runs one scenario against the C++ session and prints what the workbench shows (used by workbench_diff.py).
  add_executable(wb_probe wb_probe.cpp)
  target_link_libraries(wb_probe PRIVATE meradb_workbench meradb_core)
endif()

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
  add_test(NAME ws_text_diff
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ws_text_diff.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(ws_text_diff PROPERTIES TIMEOUT 900)
  add_test(NAME shell_session
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_session.py --cli $<TARGET_FILE:meradb_cli>)
  set_tests_properties(shell_session PROPERTIES TIMEOUT 900)
  add_test(NAME wb_cli_smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/wb_cli_smoke.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(wb_cli_smoke PROPERTIES TIMEOUT 120)
  # Pseudo-terminal tests: skip (exit 77) where Python has no pty module (Windows).
  add_test(NAME pty_shell
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/pty_shell.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(pty_shell PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
  if(TARGET wb_keyprobe)
    add_test(NAME pty_keys
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/pty_keys.py $<TARGET_FILE:wb_keyprobe>)
    add_test(NAME pty_workbench
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/pty_workbench.py $<TARGET_FILE:meradb_cli>)
    set_tests_properties(pty_keys pty_workbench PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
  endif()
  if(TARGET wb_probe)
    # Skips (exit 77) when Python has no Textual.
    add_test(NAME workbench_diff
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/workbench_diff.py --probe $<TARGET_FILE:wb_probe>)
    set_tests_properties(workbench_diff PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
  endif()
else()
  message(STATUS "Python not found: cross-engine ctest checks skipped")
endif()
