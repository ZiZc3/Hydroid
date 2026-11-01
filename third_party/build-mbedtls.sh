#!/bin/bash
# build-mbedtls.sh [ps5|pc] — mbedTLS 3.6.2 (Apache-2.0) as one static library for Hydroid's own
# HTTPS (src/https.c). Source: ~/ps5/ps5droid/third_party/mbedtls (fetched once from the
# Mbed-TLS release). Out: ~/ps5/ps5droid/build-mbedtls/<target>/libmbedtls_all.a (made once,
# again when this script or the config changes).
set -euo pipefail
target=${1:-ps5}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=${MBEDTLS_SRC:-$HOME/ps5/ps5droid/third_party/mbedtls}
out=${MBEDTLS_OUT:-$HOME/ps5/ps5droid/build-mbedtls}/$target
lib=$out/libmbedtls_all.a
if [ ! -d "$src" ]; then
    mkdir -p "$(dirname "$src")"
    curl -sL -o /tmp/mbedtls.tar.bz2 https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    tar xjf /tmp/mbedtls.tar.bz2 -C "$(dirname "$src")" && mv "$(dirname "$src")/mbedtls-3.6.2" "$src"
fi
if [ -f "$lib" ] && [ "$lib" -nt "$here/mbedtls_hydroid.h" ] && [ "$lib" -nt "$0" ]; then exit 0; fi
mkdir -p "$out/obj"
if [ "$target" = ps5 ]; then
    vk=${PS5_VULKAN:-$HOME/ps5/PS5_Vulkan}
    sdk=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
    export PS5_CLANG=${PS5_CLANG:-$(command -v clang || command -v clang-18)}
    cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18" "$@"; }
    ar=${AR:-llvm-ar-18}
    command -v "$ar" >/dev/null || ar=llvm-ar
else
    cc() { ${CC:-clang-18} "$@"; }
    ar=${AR:-ar}
fi
flags=(-O2 -I"$src/include" -I"$here" '-DMBEDTLS_USER_CONFIG_FILE="mbedtls_hydroid.h"')
[ "$target" = ps5 ] && flags+=(-march=znver2)
rm -f "$out"/obj/*.o
for f in "$src"/library/*.c; do
    cc "${flags[@]}" -c "$f" -o "$out/obj/$(basename "${f%.c}").o" &
    while [ "$(jobs -r | wc -l)" -ge 8 ]; do wait -n; done
done
wait
rm -f "$lib"
"$ar" rcs "$lib" "$out"/obj/*.o
echo "mbedtls ($target): $lib"
