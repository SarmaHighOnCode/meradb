# cpp/cmake/gui_deps.cmake -- the libraries of the desktop window (meradb_gui), fetched from pinned release files like
# nlohmann/json and FTXUI: GLFW, Dear ImGui, the Geist fonts and stb_image_write. Included from CMakeLists.txt when
# MERADB_GUI is ON (after the third-party warning helper is defined). Versions and licences: THIRD_PARTY_NOTICES.md.

# --- GLFW 3.4 (zlib licence): windows, input, OpenGL contexts ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)   # X11 only (XWayland on a Wayland desktop): fewer build requirements
endif()
FetchContent_Declare(
  glfw
  URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
  URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
)
FetchContent_MakeAvailable(glfw)
meradb_quiet_third_party(glfw)

# --- Dear ImGui 1.93.0 (MIT): has no CMake project, so its sources are compiled here ---
FetchContent_Declare(
  imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
  URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee
)
FetchContent_MakeAvailable(imgui)
add_library(meradb_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(meradb_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(meradb_imgui PUBLIC glfw ${CMAKE_DL_LIBS})
# Our own imconfig (include/meradb/gui_imconfig.h): IM_ASSERT goes through a replaceable handler so the tests can
# turn a failed assertion into an exception.
target_include_directories(meradb_imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(meradb_imgui PUBLIC IMGUI_USER_CONFIG="meradb/gui_imconfig.h" IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
if(MSVC)
  target_compile_options(meradb_imgui PRIVATE /w)
else()
  target_compile_options(meradb_imgui PRIVATE -w)
endif()

# --- Geist and Geist Mono 1.8.2 (SIL Open Font License 1.1): embedded in the program ---
FetchContent_Declare(
  geist
  URL https://github.com/vercel/geist-font/releases/download/v1.8.2/geist-font-v1.8.2.zip
  URL_HASH SHA256=43ffb66912b1c49e40c371d96e86b3b3dab42db83eb2d0c8a17321210394c4c7
)
FetchContent_MakeAvailable(geist)
set(MERADB_FONT_FILES "")
foreach(font IN ITEMS
    "kFontGeistRegular|Geist/ttf/Geist-Regular.ttf|font_geist_regular.inc"
    "kFontGeistMedium|Geist/ttf/Geist-Medium.ttf|font_geist_medium.inc"
    "kFontGeistMonoRegular|GeistMono/ttf/GeistMono-Regular.ttf|font_geist_mono_regular.inc")
  string(REPLACE "|" ";" font "${font}")
  list(GET font 0 font_var)
  list(GET font 1 font_file)
  list(GET font 2 font_inc)
  if(NOT EXISTS ${geist_SOURCE_DIR}/${font_file})
    message(FATAL_ERROR "Geist font not found: ${geist_SOURCE_DIR}/${font_file}")
  endif()
  add_custom_command(
    OUTPUT ${MERADB_GEN_DIR}/${font_inc}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${geist_SOURCE_DIR}/${font_file} -DOUTPUT=${MERADB_GEN_DIR}/${font_inc}
            -DVAR=${font_var} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${geist_SOURCE_DIR}/${font_file} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    COMMENT "Embedding ${font_file}"
    VERBATIM)
  list(APPEND MERADB_FONT_FILES ${MERADB_GEN_DIR}/${font_inc})
endforeach()

# --- stb_image_write (public domain / MIT), one header, pinned to a commit: PNG screenshots ---
FetchContent_Declare(
  stb
  URL https://raw.githubusercontent.com/nothings/stb/1ee679ca2ef753a528db5ba6801e1067b40481b8/stb_image_write.h
  URL_HASH SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb)
