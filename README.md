<p align="center">
  <img src="docs/logo.png" width="200" alt="Hydroid">
</p>

<h1 align="center">Hydroid</h1>

<p align="center">
  <b>Android games on PS5. Not an emulator.</b><br>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-PS5-2ea043" alt="platform">
  <img src="https://img.shields.io/badge/license-GPL--3.0-2ea043" alt="license">
</p>

---

## About

Hydroid loads a game's own arm64 libraries, translates its ARM code to x86-64 as it runs, and
answers every call the game makes to Android itself: the C library, the Linux kernel, the
graphics drivers, sound, input and the Java framework. There is no Android image underneath.
Each piece is written one function at a time, as games ask for it.

**The source and The engine,** its build and the tools to turn almost any Android game
  into its own PS5 app. See [Building](BUILD.md) and [Porting a game](PORTING.md).

Made for the PS5. The same code can also run on Linux or Windows.

**Only 64-bit games.** Hydroid runs 64-bit (arm64) Android games only: the game's APK must have
`lib/arm64-v8a`. Games that ship only 32-bit code don't run.

## Games

| | Game | App | Version tested | State |
|:---:|---|---|---|---|
| <img src="games/subway-surfers/icon0.png" width="64" alt="Subway Surfers"> | **[Subway Surfers](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26370.-.Subway.Surfers.zip)** | PPSA26370 | 3.70.3 | Playable |
| <img src="games/geometry-dash-lite/icon0.png" width="64" alt="Geometry Dash Lite"> | **[Geometry Dash Lite](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26371.-.Geometry.Dash.Lite.zip)** | PPSA26371 | 2.2.148 | Playable |
| <img src="games/crossy-road/icon0.png" width="64" alt="Crossy Road"> | **[Crossy Road](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26372.-.Crossy.Road.zip)** | PPSA26372 | 7.14.0 | Playable (stutters in menus) |
| <img src="games/sonic-dash/icon0.png" width="64" alt="Sonic Dash"> | **[Sonic Dash](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26375.-.Sonic.Dash.zip)** | PPSA26375 | 10.5.1 | Playable (stutters a little) |
| <img src="games/sonic-boom/icon0.png" width="64" alt="Sonic Boom"> | **[Sonic Boom](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26376.-.Sonic.Boom.zip)** | PPSA26376 | 3.26.0 | Playable |
| <img src="games/shadow-fight-2/icon0.png" width="64" alt="Shadow Fight 2"> | **[Shadow Fight 2](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26377.-.Shadow.Fight.2.zip)** | PPSA26377 | 2.46.0 | Playable |
| <img src="games/jetpack-joyride/icon0.png" width="64" alt="Jetpack Joyride"> | **[Jetpack Joyride](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26378.-.Jetpack.Joyride.zip)** | PPSA26378 | 1.104.1 | Playable |
| <img src="games/temple-run-2/icon0.png" width="64" alt="Temple Run 2"> | **[Temple Run 2](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26379.-.Temple.Run.2.zip)** | PPSA26379 | 1.137.0 | Playable |
| <img src="games/fruit-ninja/icon0.png" width="64" alt="Fruit Ninja"> | **[Fruit Ninja](https://github.com/ZiZc3/Hydroid/releases/download/1.0.0/PPSA26380.-.Fruit.Ninja.zip)** | PPSA26380 | 2.8.9 | Playable (stutters in menus) |

Every game plays with the DualSense: its own controller support where it has one, otherwise
the buttons are set up for its touch controls.

## Installing

1. Download the game's app from [Releases](../../releases) and unzip it.
2. Copy its folder to the PS5 homebrew folder:
   `/data/homebrew/`
3. Put a copy of the game (`.apk`, `.xapk`, `.apks` or `.apkm`) in the app's `game` folder:
   `/data/homebrew/<app>/game`
4. Start the game.

The apps don't include the games. use the tested versions for best compatibility.
Only the 64-bit version of a game runs: if a store offers both, pick the arm64 one.

## Built on 

[dynarmic](https://github.com/lioncash/dynarmic) (ARM JIT) ·
[FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) (JNI) ·
Mesa ([OpenGL](https://github.com/darkxex/ps5-opengl), [RADV Vulkan](https://github.com/mihawk-99/PS5_Vulkan)) ·
[ANGLE](https://chromium.googlesource.com/angle/angle) (OpenGL ES on Vulkan) ·
[astc-encoder](https://github.com/ARM-software/astc-encoder) ·
[FFmpeg](https://ffmpeg.org) (video) ·
[Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (HTTPS) ·
[Boost](https://www.boost.org) ·
[stb](https://github.com/nothings/stb) ·
[Android NDK](https://developer.android.com/ndk) ·

Fonts: [Outfit](https://github.com/Outfitio/Outfit-Fonts) and
[Noto Sans CJK](https://github.com/notofonts/noto-cjk) (SIL Open Font License) ·
icons: [Hugeicons](https://hugeicons.com) (MIT) ·
root certificates: Mozilla's, as [curl](https://curl.se/docs/caextract.html) ships them (MPL-2.0) ·
menu sound: Romain Raynal's Modern UI Vol.1 (CC0).
Each one's license is in its folder or at its link.

## Disclaimer

GPL-3.0. See [LICENSE](LICENSE).

The Android robot is reproduced or modified from work created and shared by Google and used
according to terms described in the Creative Commons 3.0 Attribution License.

Hydroid runs games you own on hardware you own. It contains no game files and no Sony or
Google code. Not affiliated with Sony, Google or any game publisher.
