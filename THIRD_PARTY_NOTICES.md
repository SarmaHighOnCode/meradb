# Third-party notices

The C++ version of MeraDB (`cpp/`) uses the open-source libraries and fonts below. Their source is **not stored in this
repository**: CMake downloads each one (pinned to the version below) when you first configure the build, with
`FetchContent` (see `cpp/CMakeLists.txt` and `cpp/cmake/gui_deps.cmake`), and unpacks it inside the build folder.
Each keeps its own licence text, which is included in the downloaded files.

| Library | Version | Licence | Upstream | Used for |
|---------|---------|---------|----------|----------|
| nlohmann/json | 3.11.3 | MIT | https://github.com/nlohmann/json | JSON in the catalog, the network protocol and the data files; linked into `meradb_cli` |
| FTXUI | 5.0.0 | MIT | https://github.com/ArthurSonzogni/FTXUI | the full-screen workbench; linked into `meradb_cli` (not downloaded when both `-DMERADB_WORKBENCH=OFF` and `-DMERADB_GUI=OFF`); `meradb_gui` uses only its text-width function |
| Catch2 | 3.5.4 | BSL-1.0 (Boost Software License 1.0) | https://github.com/catchorg/Catch2 | the unit tests only; not part of `meradb_cli` |
| GLFW | 3.4 | zlib | https://github.com/glfw/glfw | windows, input and the OpenGL context of the desktop window `meradb_gui` (not downloaded with `-DMERADB_GUI=OFF`) |
| Dear ImGui | 1.93.0 | MIT | https://github.com/ocornut/imgui | the user interface of `meradb_gui` |
| Geist and Geist Mono (fonts) | 1.8.2 | SIL Open Font License 1.1 | https://github.com/vercel/geist-font | typefaces of `meradb_gui` (Geist Regular and Medium, Geist Mono Regular); embedded in the program |
| stb_image_write | 1.16 (nothings/stb commit 1ee679ca2ef753a528db5ba6801e1067b40481b8) | public domain / MIT | https://github.com/nothings/stb | PNG screenshots of `meradb_gui` (`--screenshot`) |

On Windows the build applies a small patch to the downloaded FTXUI source (`cpp/ftxui_patch/`, applied by
`cpp/cmake/patch_ftxui.cmake`) so that emoji can be typed; FTXUI's licence applies to the patched code as well.

The Geist fonts are "Copyright 2024 The Geist Project Authors (https://github.com/vercel/geist-font)" and are
licensed under the SIL Open Font License, Version 1.1 (https://openfontlicense.org); the licence text is the file
`OFL.txt` in the font release. The fonts are embedded unmodified in the `meradb_gui` program.

The Python version (`meradb/`) uses only the Python standard library; its optional workbench uses
[Textual](https://github.com/Textualize/textual) (MIT), which is installed with `pip` and not included here.
