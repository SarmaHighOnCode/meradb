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
# The desktop window (meradb_gui): GLFW + Dear ImGui + the Geist fonts, all fetched on the first configure.
option(MERADB_GUI "Build the desktop workbench window (fetches GLFW, Dear ImGui, Geist fonts, stb_image_write)" ON)
if(MERADB_GUI AND UNIX AND NOT APPLE)
  # GLFW needs the X11 and OpenGL development files on Linux. Without them skip the window, once and clearly.
  find_package(OpenGL QUIET)
  find_package(X11 QUIET)
  set(_gui_missing "")
  if(NOT OPENGL_FOUND)
    string(APPEND _gui_missing " OpenGL (libgl1-mesa-dev)")
  endif()
  if(NOT X11_FOUND)
    string(APPEND _gui_missing " X11 (libx11-dev)")
  endif()
  foreach(_ext Xrandr Xinerama Xcursor Xi)
    if(NOT X11_${_ext}_FOUND)
      string(TOLOWER ${_ext} _ext_lower)
      string(APPEND _gui_missing " ${_ext} (lib${_ext_lower}-dev)")
    endif()
  endforeach()
  if(_gui_missing)
    message(STATUS "MERADB_GUI skipped: missing${_gui_missing}. Install them (Debian/Ubuntu: libx11-dev libxrandr-dev "
                   "libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev) or configure with -DMERADB_GUI=OFF. "
                   "The rest of MeraDB builds as usual.")
    set(MERADB_GUI OFF)
  endif()
endif()
if(MERADB_WORKBENCH OR MERADB_GUI)
  # Declared before our own warning flags below: add_compile_options only reaches targets created after it.
  # The desktop window needs only the small ftxui-screen library (string widths); EXCLUDE_FROM_ALL keeps
  # the rest of FTXUI from being built when -DMERADB_WORKBENCH=OFF.
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    ftxui
    URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(ftxui)
  if(MERADB_WORKBENCH AND WIN32)
    # FTXUI 5 drops emoji typed on Windows (it converts the two UTF-16 halves of a key event separately).
    include(cmake/patch_ftxui.cmake)
    meradb_patch_ftxui_input(${ftxui_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ftxui_patch/ftxui_win_input.h)
  endif()
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8)   # sources contain UTF-8 (Devanagari, dashes) in comments
else()
  add_compile_options(-Wall -Wextra)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    # GCC 16 speculatively devirtualizes the shared_ptr control block that nlohmann's dump() frees and picks
    # meradb::Instance (the only make_shared type in the file), then reports a false -Warray-bounds for the
    # Instance / UserStore destructors. The code is correct (all tests pass); without this option the warning
    # appears in several files that include engine.h.
    add_compile_options(-fno-devirtualize-speculatively)
  endif()
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

# The downloaded libraries are third-party code: their headers count as system headers for our targets and their
# own sources are compiled without warnings, so a build reports only what is in this repository.
function(meradb_quiet_third_party)
  foreach(name IN LISTS ARGN)
    if(NOT TARGET ${name})
      continue()
    endif()
    get_target_property(type ${name} TYPE)
    get_target_property(includes ${name} INTERFACE_INCLUDE_DIRECTORIES)
    if(includes)
      set_property(TARGET ${name} APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${includes}")
    endif()
    if(NOT type STREQUAL "INTERFACE_LIBRARY")
      if(MSVC)
        target_compile_options(${name} PRIVATE /w)
      else()
        target_compile_options(${name} PRIVATE -w)
      endif()
    endif()
  endforeach()
endfunction()
meradb_quiet_third_party(nlohmann_json Catch2 Catch2WithMain ftxui-screen ftxui-dom ftxui-component)

set(MERADB_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
if(MERADB_GUI)
  include(cmake/gui_deps.cmake)
endif()

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

if(MERADB_WORKBENCH OR MERADB_GUI)
  # docs/LANGUAGE.md is embedded at build time so the built binary needs no path to the repository.
  set(MERADB_LANGUAGE_MD ${CMAKE_CURRENT_SOURCE_DIR}/../docs/LANGUAGE.md)
  set(MERADB_EMBED_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake)
  # The file is a dependency only if it exists when CMake configures: a docs/LANGUAGE.md added later is not noticed
  # until the next configure (re-run cmake once). Editing an existing file re-embeds on the next build.
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
  # The FTXUI-free model (text, editor, tree, worker, session, markdown, help): shared by every front end.
  add_library(meradb_wbmodel STATIC
    src/wb_text.cpp
    src/wb_editor.cpp
    src/wb_tree.cpp
    src/wb_worker.cpp
    src/wb_session.cpp
    src/wb_markdown.cpp
    src/wb_help.cpp
    ${MERADB_GEN_DIR}/language_doc_data.inc
  )
  # No header of the model includes FTXUI; wb_text.cpp and wb_editor.cpp use only its string_width (terminal cell
  # widths), from the small ftxui-screen library, which therefore stays a private link dependency.
  target_link_libraries(meradb_wbmodel PUBLIC meradb_core PRIVATE ftxui::screen)
  target_include_directories(meradb_wbmodel SYSTEM PRIVATE ${ftxui_SOURCE_DIR}/include)
  target_include_directories(meradb_wbmodel PRIVATE ${MERADB_GEN_DIR})
endif()

if(MERADB_WORKBENCH)
  add_library(meradb_workbench STATIC
    src/workbench.cpp
    src/wb_panels.cpp
    src/wb_ui.cpp
    src/wb_form.cpp
    src/wb_dialogs.cpp
    # workbench source files are appended here by later tasks
  )
  target_link_libraries(meradb_workbench PUBLIC meradb_wbmodel meradb_core ftxui::component ftxui::dom ftxui::screen)
  # FTXUI's headers are third-party: keep our warning flags quiet about them.
  target_include_directories(meradb_workbench SYSTEM PUBLIC ${ftxui_SOURCE_DIR}/include)
endif()

if(MERADB_GUI)
  # The window, as a library (everything but main) so the tests can link it, and the program.
  add_library(meradb_gui_lib STATIC
    src/gui_theme.cpp
    src/gui_poster.cpp
    src/gui_options.cpp
    src/gui_fonts.cpp
    src/gui_gl.cpp
    src/gui_editor.cpp
    src/gui_tree.cpp
    src/gui_grid.cpp
    src/gui_ui.cpp
    src/gui_window.cpp
    src/gui_app.cpp
    ${MERADB_FONT_FILES}
  )
  target_link_libraries(meradb_gui_lib PUBLIC meradb_wbmodel meradb_core meradb_imgui glfw)
  target_include_directories(meradb_gui_lib PRIVATE ${MERADB_GEN_DIR})
  target_include_directories(meradb_gui_lib SYSTEM PRIVATE ${stb_image_write_SOURCE_DIR})
  if(WIN32)
    target_link_libraries(meradb_gui_lib PUBLIC advapi32 user32)
  endif()

  add_executable(meradb_gui src/gui_main.cpp)
  target_link_libraries(meradb_gui PRIVATE meradb_gui_lib)
  if(WIN32)
    if(MSVC)
      target_link_options(meradb_gui PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
      target_link_options(meradb_gui PRIVATE -mwindows)   # no console window when double-clicked
    endif()
  endif()
  if(MINGW)
    target_link_options(meradb_gui PRIVATE -static)   # the MinGW runtime inside the program, like meradb_cli
  endif()
endif()

add_executable(meradb_cli src/main.cpp)
target_link_libraries(meradb_cli PRIVATE meradb_core)
if(MERADB_WORKBENCH)
  target_link_libraries(meradb_cli PRIVATE meradb_workbench)
  target_compile_definitions(meradb_cli PRIVATE MERADB_HAVE_WORKBENCH=1)
endif()
if(MINGW)
  # Link the MinGW runtime into the program so it starts from any terminal,
  # without the compiler's bin folder on PATH.
  target_link_options(meradb_cli PRIVATE -static)
endif()

enable_testing()
add_subdirectory(tests)
