# Building Hydroid

Hydroid builds on Linux (Ubuntu 24.04, or WSL2 on Windows). The result is one PS5 app,
`dist/PPSA00000`: the engine. Each game app is a copy of it with the game's name, icon and
settings (see [PORTING.md](PORTING.md)).

## 1. What you need

| | Where | Set with |
|---|---|---|
| clang 18, cmake, ninja, python3, git, curl | `sudo apt install clang-18 lld-18 llvm-18 cmake ninja-build python3 python3-pip git curl` | |
| Pillow, etcpak | `pip install pillow etcpak` | |
| Android NDK r27c | [developer.android.com/ndk](https://developer.android.com/ndk/downloads) | `ANDROID_NDK` |
| PS5_Vulkan (its toolchain, payload SDK, signing tool and runtime) | [Mihawk-99/PS5_Vulkan](https://github.com/Mihawk-99/PS5_Vulkan), built as its README says | `PS5_VULKAN` |
| ps5-opengl SDK 1.0.0 | [ps5-opengl](https://github.com/darkxex/ps5-opengl) release | `PS5_OPENGL_PREFIX` (its `sdk` folder) |
| FFmpeg, built for the PS5 as static libraries | your own PS5 build of FFmpeg (`lib/` and `include/`) | `FFMPEG_PREFIX` |
| dynarmic source | [dynarmic](https://github.com/lioncash/dynarmic) | `DYNARMIC_SRC` |
| astc-encoder 4.8.0 source | [ARM-software/astc-encoder](https://github.com/ARM-software/astc-encoder) (tag 4.8.0) | `ASTC_SRC` |
| ANGLE (optional: OpenGL ES on Vulkan) | [ANGLE](https://chromium.googlesource.com/angle/angle), then `python3 third_party/build-angle-ps5.py` | `ANGLE_LIB` |

The build fetches the rest itself: Boost (`third_party/fetch-boost.sh`) and mbedTLS
(`third_party/build-mbedtls.sh`).

Every path has a default under `~/ps5/` (see the top of `title/build-title.sh`); set the
variables above when yours are elsewhere.

## 2. Build the engine

```bash
bash third_party/fetch-boost.sh
bash title/build-title.sh
```

The first build also builds dynarmic and astc-encoder for the PS5 (a few minutes). The engine
is then in `dist/PPSA00000`, ending with a line like:

```
==> PPSA00000: dist/PPSA00000 (eboot.bin 79509223 bytes, 472 imports)
```

The build stops if the engine would call a function the PS5 doesn't have
(`title/console-missing.txt`).

## 3. Make a game's app

```bash
python3 tools/make_app.py games/temple-run-2 --engine dist/PPSA00000
```

This makes `dist/PPSA26379`: the engine with that game's title id, name, icon and background.

## 4. Put it on the PS5

1. Copy the app's folder (e.g. `dist/PPSA26379`) to `/data/homebrew/` on the console (FTP).
2. Put your copy of the game (`.apk`, `.xapk`, `.apks` or `.apkm`) in its `game` folder.
3. Start it from the home screen.

After a new build, `PS5=<console IP> bash tools/upload.sh PPSA26379` sends only what changed.

## 5. Read the log

Each app writes `android.log` in its folder (`/data/homebrew/<title id>/android.log`), and
streams it live while it runs:

```bash
python3 tools/live_log.py --host <console IP>
```
