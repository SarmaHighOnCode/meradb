#!/usr/bin/env sh
# Build the C++ MeraDB (cpp/) in Release mode on Linux or macOS.
#
#   ./build.sh                 configure + build into cpp/build
#   ./build.sh --test          ... then run ctest
#   ./build.sh --no-workbench  skip FTXUI (nothing is downloaded for it)
#   ./build.sh --dry-run       only print what would be run
#   BUILD_DIR=/tmp/mb ./build.sh   use another build folder
set -eu
cd "$(dirname "$0")"

build_dir="${BUILD_DIR:-cpp/build}"
test=0; dry=0; workbench=ON
for arg in "$@"; do
    case "$arg" in
        --test) test=1 ;;
        --dry-run) dry=1 ;;
        --no-workbench) workbench=OFF ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

command -v cmake >/dev/null 2>&1 || { echo "cmake not found. Install CMake 3.20 or newer." >&2; exit 1; }
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

run() {
    echo "> $*"
    [ "$dry" -eq 1 ] || "$@"
}

run cmake -S cpp -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DMERADB_WORKBENCH="$workbench"
run cmake --build "$build_dir" --parallel "$jobs"
[ "$test" -eq 0 ] || run ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs"

echo "Done. Program: $build_dir/meradb_cli"
