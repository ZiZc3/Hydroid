#!/usr/bin/env bash
# astcenc (ARM-software/astc-encoder 4.8.0, Apache-2.0) for the host: the decoder only, AVX2.
# src/astc_native.cpp hands it Unity's ASTC textures (libunity's own copy runs in the JIT).
#   bash third_party/build-astc.sh pc|ps5 <out dir> [compiler...]
# PC: clang++-18. PS5: the title's prospero-clang (passed as the compiler command).
set -euo pipefail
target=$1
out=$2
shift 2
src=${ASTC_SRC:-$HOME/ps5/ps5droid/third_party/astc-encoder}
if [[ ! -d $src/Source ]]; then
    git clone -q --depth 1 -b 4.8.0 https://github.com/ARM-software/astc-encoder.git "$src"
fi
[[ -f $out/libastcenc.a && $out/libastcenc.a -nt $0 ]] && exit 0
mkdir -p "$out/obj"
flags=(-std=c++17 -O2 -DNDEBUG -DASTCENC_DECOMPRESSOR=1 -DASTCENC_SSE=41 -DASTCENC_AVX=2
    -DASTCENC_POPCNT=1 -DASTCENC_F16C=1 -DASTCENC_NEON=0 -DASTCENC_SVE=0 -w)
if [[ $target == ps5 ]]; then flags+=(-march=znver2 -fexceptions -fcxx-exceptions)
else flags+=(-mavx2 -mfma -mf16c -mpopcnt -msse4.1); fi
objs=()
for f in "$src"/Source/astcenc_*.cpp; do
    o=$out/obj/$(basename "${f%.cpp}").o
    "$@" "${flags[@]}" -c "$f" -o "$o"
    objs+=("$o")
done
rm -f "$out/libastcenc.a"
${AR:-llvm-ar-18} rcs "$out/libastcenc.a" "${objs[@]}"
echo "astcenc: $out/libastcenc.a"
