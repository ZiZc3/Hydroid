<p align="center">
  <img src="docs/logo.png" width="200" alt="Hydroid">
</p>

<h1 align="center">Hydroid</h1>

<p align="center">
  <b>Android games on PS5.</b><br>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-PS5-2ea043" alt="platform">
  <img src="https://img.shields.io/badge/license-GPL--3.0-2ea043" alt="license">
</p>

---

## About

Each game in the collection is a separate PS5 app with its own name, icon and background.
There is no launcher and no menu in between: the app opens straight into the game.

Each app is made and tested for its one game, so an update for one game never breaks another.

## Games

| | Game | App | Version tested | State |
|:---:|---|---|---|---|
| <img src="games/subway-surfers/icon0.png" width="64" alt="Subway Surfers"> | **Subway Surfers** | PPSA26370 | 3.70.3 | Playable |
| <img src="games/geometry-dash-lite/icon0.png" width="64" alt="Geometry Dash Lite"> | **Geometry Dash Lite** | PPSA26371 | 2.2.148 | Playable |
| <img src="games/crossy-road/icon0.png" width="64" alt="Crossy Road"> | **Crossy Road** | PPSA26372 | 7.14.0 | Playable (stutters in menus) |
| <img src="games/sonic-dash/icon0.png" width="64" alt="Sonic Dash"> | **Sonic Dash** | PPSA26375 | 10.5.1 | Playable (stutters a little) |
| <img src="games/sonic-boom/icon0.png" width="64" alt="Sonic Boom"> | **Sonic Boom** | PPSA26376 | 3.26.0 | Playable |
| <img src="games/shadow-fight-2/icon0.png" width="64" alt="Shadow Fight 2"> | **Shadow Fight 2** | PPSA26377 | 2.46.0 | Playable |
| <img src="games/jetpack-joyride/icon0.png" width="64" alt="Jetpack Joyride"> | **Jetpack Joyride** | PPSA26378 | 1.104.1 | Playable |
| <img src="games/temple-run-2/icon0.png" width="64" alt="Temple Run 2"> | **Temple Run 2** | PPSA26379 | 1.137.0 | Playable |
| <img src="games/fruit-ninja/icon0.png" width="64" alt="Fruit Ninja"> | **Fruit Ninja** | PPSA26380 | 2.8.9 | Playable |

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

## Built on

[dynarmic](https://github.com/lioncash/dynarmic) (ARM JIT) ·
[FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) (JNI) ·
Mesa (OpenGL, RADV Vulkan) ·
[astc-encoder](https://github.com/ARM-software/astc-encoder) ·
[stb](https://github.com/nothings/stb).

## Disclaimer

Hydroid runs games you own on hardware you own. It contains no game files and no Sony or
Google code. Not affiliated with Sony, Google or any game publisher.

## License

GPL-3.0. See [LICENSE](LICENSE).

The Android robot is reproduced or modified from work created and shared by Google and used
according to terms described in the Creative Commons 3.0 Attribution License.
