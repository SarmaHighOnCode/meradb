# Third-party notices

The C++ version of MeraDB (`cpp/`) uses three open-source libraries. Their source is **not stored in this
repository**: CMake downloads each one (pinned to the version below) when you first configure the build, with
`FetchContent` (see `cpp/CMakeLists.txt`), and unpacks it inside the build folder. Each library keeps its own
licence text, which is included in the downloaded source.

| Library | Version | Licence | Upstream | Used for |
|---------|---------|---------|----------|----------|
| nlohmann/json | 3.11.3 | MIT | https://github.com/nlohmann/json | JSON in the catalog, the network protocol and the data files; linked into `meradb_cli` |
| FTXUI | 5.0.0 | MIT | https://github.com/ArthurSonzogni/FTXUI | the full-screen workbench; linked into `meradb_cli` (not downloaded with `-DMERADB_WORKBENCH=OFF`) |
| Catch2 | 3.5.4 | BSL-1.0 (Boost Software License 1.0) | https://github.com/catchorg/Catch2 | the unit tests only; not part of `meradb_cli` |

On Windows the build applies a small patch to the downloaded FTXUI source (`cpp/ftxui_patch/`, applied by
`cpp/cmake/patch_ftxui.cmake`) so that emoji can be typed; FTXUI's licence applies to the patched code as well.

The Python version (`meradb/`) uses only the Python standard library; its optional workbench uses
[Textual](https://github.com/Textualize/textual) (MIT), which is installed with `pip` and not included here.
