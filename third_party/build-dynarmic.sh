#!/usr/bin/env bash
# dynarmic (suyu-emu/dynarmic, 0BSD): ARM64 -> x86-64 JIT, as static libraries
# for our PS5 title in ~/ps5/ps5droid/build-dynarmic.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$here")
src=${DYNARMIC_SRC:-$HOME/ps5/ps5droid/third_party/dynarmic}
boost=${BOOST_DIR:-$HOME/ps5/ps5droid/third_party/boost}
out=${DYNARMIC_OUT:-$HOME/ps5/ps5droid/build-dynarmic}
python3 "$here/patches/dynarmic-ps5.py" "$src"
# CMake 4 has no FindBoost: a config for the header-only use dynarmic makes.
mkdir -p "$out/boost-config"
cat > "$out/boost-config/BoostConfig.cmake" <<CFG
set(Boost_FOUND TRUE)
set(Boost_VERSION 1.86.0)
set(Boost_INCLUDE_DIRS "$boost")
if(NOT TARGET Boost::boost)
  add_library(Boost::boost INTERFACE IMPORTED)
  set_target_properties(Boost::boost PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "$boost")
endif()
if(NOT TARGET Boost::headers)
  add_library(Boost::headers ALIAS Boost::boost)
endif()
CFG
printf 'set(PACKAGE_VERSION 1.86.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n' > "$out/boost-config/BoostConfigVersion.cmake"
chmod +x "$repo/title/toolchain/ps5-cc" "$repo/title/toolchain/ps5-c++"
cmake -S "$src" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$repo/title/toolchain/ps5.cmake" \
    -DBoost_DIR="$out/boost-config" \
    -DDYNARMIC_FRONTENDS="A64;A32" -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON \
    -DDYNARMIC_WARNINGS_AS_ERRORS=OFF -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-march=znver2 -fexceptions -fcxx-exceptions" > "$out/configure.log" 2>&1 ||
    { tail -30 "$out/configure.log"; exit 1; }
ninja -C "$out" dynarmic 2>&1 | tail -5
find "$out" -name '*.a' | sort
