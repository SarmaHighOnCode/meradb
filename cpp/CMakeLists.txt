# cpp/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(meradb_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
if(MSVC)
  add_compile_options(/W4 /utf-8)   # sources contain UTF-8 (Devanagari, dashes) in comments
else()
  add_compile_options(-Wall -Wextra)
endif()

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get the time of extraction (silences FetchContent warning)
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

add_executable(meradb_cli src/main.cpp)
target_link_libraries(meradb_cli PRIVATE meradb_core)

enable_testing()
add_subdirectory(tests)
