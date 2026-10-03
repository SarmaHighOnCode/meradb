# cpp/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(meradb_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get the time of extraction (silences FetchContent warning)
endif()

# The full-screen workbench needs FTXUI, downloaded on the first configure. Without network access configure with
# -DMERADB_WORKBENCH=OFF (the rest builds as before and `workbench` prints a note), or point
# -DFETCHCONTENT_SOURCE_DIR_FTXUI=<unpacked v5.0.0 tarball> at a copy you already have.
option(MERADB_WORKBENCH "Build the full-screen workbench (fetches FTXUI)" ON)
if(MERADB_WORKBENCH)
  # Declared before our own warning flags below: add_compile_options only reaches targets created after it.
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    ftxui
    URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz
  )
  FetchContent_MakeAvailable(ftxui)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8)   # sources contain UTF-8 (Devanagari, dashes) in comments
else()
  add_compile_options(-Wall -Wextra)
endif()

FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
  catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.5.4.tar.gz
)
FetchContent_MakeAvailable(catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_library(meradb_core STATIC
  src/stack_guard.cpp
  src/datatypes.cpp
  src/tokenizer.cpp
  src/ast_util.cpp
  src/parser.cpp
  src/catalog.cpp
  src/storage.cpp
  src/table.cpp
  src/planner.cpp
  src/pyvalue.cpp
  src/evaluator.cpp
  src/aggregates.cpp
  src/engine.cpp
  src/cli_format.cpp
  src/sys_compat.cpp
  src/crypto.cpp
  src/users.cpp
  src/substitute.cpp
  src/net_compat.cpp
  src/pyjson.cpp
  src/protocol.cpp
  src/server.cpp
  src/client.cpp
  src/server_control.cpp
  src/cli.cpp
  src/pytext.cpp
  src/term_style.cpp
  src/repl_text.cpp
  src/repl.cpp
  src/highlight.cpp
  # source files are appended here by later tasks
)
if(NOT MSVC)
  # 100,000 PBKDF2 iterations = 200,000 SHA-256 compressions per login; -O0 is ~10x too slow.
  set_source_files_properties(src/crypto.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()
target_include_directories(meradb_core PUBLIC include)
target_link_libraries(meradb_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
if(WIN32)
  # Winsock + the OS random generator; keep windows.h lean and min/max macros away.
  target_link_libraries(meradb_core PUBLIC ws2_32 bcrypt shell32)
  target_compile_definitions(meradb_core PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MERADB_WORKBENCH)
  # docs/LANGUAGE.md is embedded at build time so the built binary needs no path to the repository.
  set(MERADB_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  set(MERADB_LANGUAGE_MD ${CMAKE_CURRENT_SOURCE_DIR}/../docs/LANGUAGE.md)
  set(MERADB_EMBED_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake)
  if(EXISTS ${MERADB_LANGUAGE_MD})
    list(APPEND MERADB_EMBED_DEPENDS ${MERADB_LANGUAGE_MD})
  endif()
  add_custom_command(
    OUTPUT ${MERADB_GEN_DIR}/language_doc_data.inc
    COMMAND ${CMAKE_COMMAND} -DINPUT=${MERADB_LANGUAGE_MD} -DOUTPUT=${MERADB_GEN_DIR}/language_doc_data.inc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${MERADB_EMBED_DEPENDS}
    COMMENT "Embedding docs/LANGUAGE.md"
    VERBATIM)
  add_library(meradb_workbench STATIC
    src/workbench.cpp
    src/wb_text.cpp
    src/wb_editor.cpp
    src/wb_tree.cpp
    src/wb_worker.cpp
    src/wb_session.cpp
    src/wb_markdown.cpp
    src/wb_help.cpp
    ${MERADB_GEN_DIR}/language_doc_data.inc
    # workbench source files are appended here by later tasks
  )
  target_link_libraries(meradb_workbench PUBLIC meradb_core ftxui::component ftxui::dom ftxui::screen)
  # FTXUI's headers are third-party: keep our warning flags quiet about them.
  target_include_directories(meradb_workbench SYSTEM PUBLIC ${ftxui_SOURCE_DIR}/include)
  target_include_directories(meradb_workbench PRIVATE ${MERADB_GEN_DIR})
endif()

add_executable(meradb_cli src/main.cpp)
target_link_libraries(meradb_cli PRIVATE meradb_core)
if(MERADB_WORKBENCH)
  target_link_libraries(meradb_cli PRIVATE meradb_workbench)
  target_compile_definitions(meradb_cli PRIVATE MERADB_HAVE_WORKBENCH=1)
endif()

enable_testing()
add_subdirectory(tests)
