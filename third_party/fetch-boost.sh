#!/usr/bin/env bash
# Boost headers for dynarmic (it uses boost/icl and boost/variant, header-only).
set -euo pipefail
dest=${1:-$HOME/ps5/ps5droid/third_party/boost}
ver=1.86.0
tar=boost_${ver//./_}.tar.gz
dl=$HOME/ps5/ps5droid/third_party/dl
mkdir -p "$dl" "$dest"
[[ -f $dl/$tar ]] || curl -sSL --max-time 900 -o "$dl/$tar" "https://archives.boost.io/release/$ver/source/$tar"
tar -xzf "$dl/$tar" -C "$dest" --strip-components=1 "boost_${ver//./_}/boost" "boost_${ver//./_}/LICENSE_1_0.txt"
grep 'define BOOST_LIB_VERSION' "$dest/boost/version.hpp"
