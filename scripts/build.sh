#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_type="${BUILD_TYPE:-Release}"

command -v conan >/dev/null || { echo "conan is required" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 1; }

if ! conan profile path default >/dev/null 2>&1; then
  conan profile detect
fi

conan install "$root_dir" \
  --output-folder="$root_dir/build" \
  --build=missing \
  --settings="build_type=$build_type"

toolchain="$root_dir/build/build/$build_type/generators/conan_toolchain.cmake"
cmake --fresh -S "$root_dir" -B "$root_dir/build/cmake-$build_type" \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DOTEL_METRICS_BUILD_TESTING=ON
cmake --build "$root_dir/build/cmake-$build_type" --parallel
ctest --test-dir "$root_dir/build/cmake-$build_type" --output-on-failure
