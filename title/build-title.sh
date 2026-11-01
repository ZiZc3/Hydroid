#!/usr/bin/env bash
# Build Android as a PS5 title: dist/<titleId>/ with a signed eboot.bin.
# Same link as VLC's ps5/build-title.sh (PS5_Vulkan's title recipe), no RADV;
# OpenGL from ps5-opengl (blackbearreloaded/ps5-opengl SDK, GPL-3.0-or-later).
#
#   bash title/build-title.sh                       (test game)
#   GAME=endless-tunnel bash title/build-title.sh   (+ games/<name>: lib/libgame.so)
#   GLES=stub ...                                   (no GPU: phase 4a's stand-in)
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$here")
vk=${PS5_VULKAN:-$HOME/ps5/PS5_Vulkan}
sdk=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
native=$vk/tooling/native
tool=$vk/build/host/ps5-native-tool
param=$here/sce_sys/param.json
work=${ANDROID_WORK:-$HOME/ps5/ps5droid/build-title}
ndk=${ANDROID_NDK:-$HOME/ps5/ps5droid/android-ndk-r27c}
ndk_cc=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
gl_sdk=${PS5_OPENGL_PREFIX:-$HOME/ps5/ps5-opengl-sdk/ps5-opengl-sdk-1.0.0/sdk}
dyn_src=${DYNARMIC_SRC:-$HOME/ps5/ps5droid/third_party/dynarmic}
dyn_out=${DYNARMIC_OUT:-$HOME/ps5/ps5droid/build-dynarmic}
boost=${BOOST_DIR:-$HOME/ps5/ps5droid/third_party/boost}
zlib=${ZLIB_DIR:-$vk/.deps/native/zlib/zlib-1.3.2}
gles=${GLES:-ps5-opengl}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || command -v clang-18)}

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18" "$@"; }

rm -rf "$work"
mkdir -p "$work/obj"

# 1. The Android side: the test game, built by the NDK against the real
#    libandroid/libaaudio (their calls are answered by src/android_shim.c).
"$ndk_cc" --target=x86_64-linux-android35 -shared -fPIC -nostdlib -O2 \
    -fno-stack-protector -fvisibility=hidden -Wall \
    -o "$work/libdroid_game.so" "$repo/src/droid_game.c" -landroid -laaudio -llog
# ... and an ARM64 library, for the JIT (dynarmic).
"$ndk_cc" --target=aarch64-linux-android35 -shared -fPIC -nostdlib -O2 -fvisibility=hidden \
    -fstack-protector-strong -mno-outline-atomics -o "$work/libtest_arm64.so" "$repo/src/test_android_lib.c" \
    -lc -lm -llog
[[ -f $dyn_out/src/dynarmic/libdynarmic.a ]] || bash "$repo/third_party/build-dynarmic.sh"

# 2. The PS5 side, in an archive so --exclude-libs=ALL keeps it out of the exports.
fjni=$repo/third_party/FalsoJNI
# mbedTLS for Hydroid's own HTTPS (src/https.c; built once by third_party/build-mbedtls.sh)
mbedtls_src=${MBEDTLS_SRC:-$HOME/ps5/ps5droid/third_party/mbedtls}
bash "$repo/third_party/build-mbedtls.sh" ps5
mbedtls_lib=${MBEDTLS_OUT:-$HOME/ps5/ps5droid/build-mbedtls}/ps5/libmbedtls_all.a
# FFmpeg for the video player (src/mediandk.c): the VLC port's PS5 build (LGPL, decoders + demuxers only)
ffmpeg=${FFMPEG_PREFIX:-$HOME/ps5/vlc-ps5/prefix-ps5}
ffmpeg_libs=("$ffmpeg/lib/libavformat.a" "$ffmpeg/lib/libavcodec.a" "$ffmpeg/lib/libswscale.a"
    "$ffmpeg/lib/libswresample.a" "$ffmpeg/lib/libavutil.a")
cflags=(-O2 -march=znver2 -Wall -std=gnu11 -I"$repo/src" -I"$fjni" -I"$repo/src/compat" -I"$mbedtls_src/include"
    -I"$repo/third_party" -idirafter "$ffmpeg/include" '-DMBEDTLS_USER_CONFIG_FILE="mbedtls_hydroid.h"')
cc "${cflags[@]}" -c "$repo/src/app_main.c" -o "$work/obj/app_main.o"
cc "${cflags[@]}" -c "$repo/src/soloader.c" -o "$work/obj/soloader.o"
vk_include=$vk/.deps/native/radv-release/include
for f in android_shim imangi_xls texspy phone_gl https game_backup firebase_rc mediandk video_tex video_player gameactivity_host write_behind big_files thread_place libc_extra gtasa_host cocos_host mortar_host bionic_shim native_activity display opensl arm_imports linux_abi apk godot_host \
    arm_modules unity_host java_layer vulkan_bridge overlay_stats quick_menu ui_icons sensors dpad_grid keymap vk_ps5 overlay_art game_settings ui_gl ui_font ui_sound game_file net_shim libc_missing unity_patch apk_sign arm_trace; do
    cc "${cflags[@]}" -I"$zlib" -I"$vk_include" -I"$gl_sdk/include" -I"$repo/third_party/stb" -c "$repo/src/$f.c" -o "$work/obj/$f.o"
done
gl_inputs=()
gl_stubs=()
if [[ $gles == stub ]]; then
    cc "${cflags[@]}" -c "$repo/src/egl_stub.c" -o "$work/obj/egl_stub.o"
else
    cc "${cflags[@]}" -I"$gl_sdk/include" -c "$repo/src/gles_bridge.c" -o "$work/obj/gles_bridge.o"
    # AGC comes from system modules; link stubs name its functions (.prx sonames).
    mkdir -p "$work/stubs"
    for library in libSceAgc libSceAgcDriver; do
        llvm-nm-18 -D --defined-only "$gl_sdk/lib/$library.so" |
            awk '$2 == "T" || $2 == "W" { printf "void %s(void) {}\n", $3 }' > "$work/${library}_stub.c"
        # RADV's AGC driver calls that ps5-opengl doesn't make
        if [[ $library == libSceAgcDriver ]] && ! grep -q sceAgcDriverSetHsOffchipParam "$work/${library}_stub.c"; then
            echo "void sceAgcDriverSetHsOffchipParam(void) {}" >> "$work/${library}_stub.c"
        fi
        cc -std=c11 -O2 -fPIC -c "$work/${library}_stub.c" -o "$work/${library}_stub.o"
        "$sdk/bin/prospero-lld" --shared -soname "$library.prx" \
            -o "$work/stubs/$library.so" "$work/${library}_stub.o"
        gl_stubs+=(--stub "$work/stubs/$library.so")
    done
    gl_inputs=("$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so"
        -u ps5_agc_gate2_run -u eglGetDisplay -u eglInitialize -u eglSwapBuffers
        -u eglGetProcAddress -L "$gl_sdk/lib" --start-group "$gl_sdk/lib/libPS5OpenGL.a" --end-group)
fi
# ANGLE (OpenGL ES on Vulkan, third_party/build-angle-ps5.py): in when its archive is there; ANGLE=0 leaves it out
angle_lib=${ANGLE_LIB:-$HOME/ps5/angle/out/ps5/libangle_ps5.a}
angle_inputs=()
if [[ ${ANGLE:-1} != 0 && -f $angle_lib ]]; then
    cc "${cflags[@]}" -DDROID_ANGLE -c "$repo/src/angle_gles.c" -o "$work/obj/angle_gles.o"
    angle_inputs=(-u angle_resolve "$angle_lib")
    echo "==> ANGLE in ($angle_lib)"
else
    cc "${cflags[@]}" -c "$repo/src/angle_gles.c" -o "$work/obj/angle_gles.o"
fi
cc "${cflags[@]}" -c "$repo/src/jni_impl.c" -o "$work/obj/jni_impl.o"
cc -std=c++20 -O2 -march=znver2 -Wall -fexceptions -fcxx-exceptions -I"$repo/src" \
    -I"$dyn_src/src" -I"$dyn_src/externals/mcl/include" -I"$boost" \
    -c "$repo/src/arm_cpu.cpp" -o "$work/obj/arm_cpu.o"
cc "${cflags[@]}" -c "$repo/src/fjni_logger.c" -o "$work/obj/fjni_logger.o"
# astcenc for Unity's ASTC textures (src/astc_native.cpp): its objects go in libapp.a
astc_src=${ASTC_SRC:-$HOME/ps5/ps5droid/third_party/astc-encoder}
astc_out=${ASTC_PS5_OUT:-$HOME/ps5/ps5droid/build-astc-ps5}
AR="$sdk/bin/prospero-ar" bash "$repo/third_party/build-astc.sh" ps5 "$astc_out" \
    env PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18"
cp "$astc_out"/obj/*.o "$work/obj/"
cc -std=c++17 -O2 -march=znver2 -fexceptions -fcxx-exceptions -I"$astc_src/Source" \
    -c "$repo/src/astc_native.cpp" -o "$work/obj/astc_native.o"
cc -std=c++17 -O2 -march=znver2 -c "$repo/src/etc_native.cpp" -o "$work/obj/etc_native.o"
# FalsoJNI (MIT, v-atamanenko/FalsoJNI 083d5a0), minus its Vita logger.
for f in FalsoJNI FalsoJNI_ImplBridge converter; do
    cc "${cflags[@]}" -Wno-format -Wno-unused-variable -Wno-return-type -c "$fjni/$f.c" -o "$work/obj/fjni_$f.o"
done
"$sdk/bin/prospero-ar" rcs "$work/libapp.a" "$work"/obj/*.o
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_cpp_runtime.cpp" -o "$work/app_cpp_runtime.o"

# 3. Link with the title recipe and RADV (Vulkan games), its Mesa names made private so
#    it sits beside ps5-opengl's Mesa (title/isolate-radv.py, ProsperoEden's method).
radv_src=$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
radv_iso=${RADV_ISOLATED:-$HOME/ps5/ps5droid/build-radv-isolated}
if [[ ! -f $radv_iso/libvulkan_radeon.ps5.a || $radv_src -nt $radv_iso/libvulkan_radeon.ps5.a ]]; then
    python3 "$repo/title/isolate-radv.py" "$radv_src" "$radv_iso" || exit 2
fi
# shellcheck source=/dev/null
source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk" "$radv_iso/libvulkan_radeon.ps5.a" || exit 2
inputs=()
for item in "${radv_link_inputs[@]}"; do # RADV whole (its dispatch tables name everything weakly)
    inputs+=("$item")
done
inputs+=(-u vk_icdGetInstanceProcAddr)
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --error-limit=0 \
    --eh-frame-hdr --no-dynamic-linker -z nodynamic-undefined-weak "${radv_link_flags[@]}" \
    --wrap=sceVideoOutOpen --wrap=sceVideoOutSetFlipRate \
    --wrap=readlink --wrap=isatty --wrap=fnmatch --wrap=mkstemp \
    --wrap=coshf --wrap=acoshf --wrap=asinhf --wrap=atanhf --wrap=fgetwc --wrap=fputwc --wrap=ungetwc --wrap=getrusage \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -Map="$work/eboot.map" -o "$work/llvm-pie.elf" \
    "$work/app_crt.o" "$work/app_cpp_runtime.o" \
    --start-group "$work/libapp.a" "$dyn_out/src/dynarmic/libdynarmic.a" \
    "$dyn_out/externals/mcl/src/libmcl.a" "$dyn_out/externals/fmt/libfmt.a" \
    "$dyn_out/externals/zydis/libZydis.a" "$dyn_out/externals/zydis/zycore/libZycore.a" "$zlib/libz.a" \
    "$mbedtls_lib" "${ffmpeg_libs[@]}" "${angle_inputs[@]}" --end-group \
    "${gl_inputs[@]}" \
    "${inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so 2>&1 | tee "$work/link.log" | grep -E "error|warning" | head -40
[[ ${PIPESTATUS[0]} -eq 0 && -f $work/llvm-pie.elf ]] || { echo "link failed: $work/link.log" >&2; exit 1; }

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk/target/lib" "${gl_stubs[@]}" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

# 4. The title folder.
app=$repo/dist/$title_id
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/lib"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$param" "$app/sce_sys/param.json"
mkdir -p "$app/assets" "$app/game" && cp -r "$here/assets/fonts" "$here/assets/certs" "$app/assets/" # the menus' fonts, HTTPS's root certificates
for asset in icon0.png pic0.dds pic1.dds; do
    if [[ -f $here/sce_sys/$asset ]]; then cp "$here/sce_sys/$asset" "$app/sce_sys/$asset"
    else cp "$vk/sce_sys/$asset" "$app/sce_sys/$asset"; fi
done
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
cp "$work/libdroid_game.so" "$work/libtest_arm64.so" "$app/lib/"
# APK=<file.apk>: the game itself, read in place on the console (/app0/game.apk)
if [[ -n ${APK:-} ]]; then
    case $APK in
        *.xapk) cp "$APK" "$app/game.xapk" ;;
        *.apks) cp "$APK" "$app/game.apks" ;;
        *) cp "$APK" "$app/game.apk" ;;
    esac
fi
if [[ -n ${GAME:-} ]]; then
    GAME_OUT=$work/game bash "$repo/games/$GAME/build.sh"
    cp "$work/game/libgame.so" "$app/lib/libgame.so"
fi
# the imports' slots, and main's own address first (the app finds its load address by it
# and logs any slot the console left at 0: a function its libc lacks)
{
    llvm-nm-18 "$work/llvm-pie.elf" | awk '$3 == "main" && !done { print $1, "main"; done = 1 }'
    llvm-readelf-18 -r --wide "$work/llvm-pie.elf" |
        awk '$3 == "R_X86_64_GLOB_DAT" || $3 == "R_X86_64_JUMP_SLOT" { print $1, $5 }'
} > "$app/imports.txt"
# a function the console lacks (title/console-missing.txt): a crash on the PS5 that the PC never
# shows (strcasestr, 2026-10-29). The build stops here.
bad=$(awk 'NR == FNR { if ($1 !~ /^#/ && $1 != "") miss[$1] = 1; next } ($2 in miss) { print $2 }' \
    "$here/console-missing.txt" "$app/imports.txt" | sort -u)
if [[ -n $bad ]]; then
    echo "!! Hydroid's code imports what the PS5 doesn't have (it would crash there): $bad" >&2
    echo "!! use Hydroid's own version (e.g. droid_strcasestr) instead" >&2
    exit 1
fi
printf '==> %s: %s (eboot.bin %s bytes, %s imports)\n' "$title_id" "$app" \
    "$(stat -c %s "$app/eboot.bin")" "$(wc -l < "$app/imports.txt")"
