<p align="center">
  <img src="docs/logo.png" width="200" alt="PS5 Android Collection">
</p>

<h1 align="center">PS5 Android Collection</h1>

<p align="center">
  <b>Android games on PS5.</b><br>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-PS5-2ea043" alt="platform">
  <img src="https://img.shields.io/badge/games-1-2ea043" alt="games">
  <img src="https://img.shields.io/badge/license-GPL--3.0-2ea043" alt="license">
</p>

---

## About

Each game in the collection is a separate PS5 app with its own name, icon and background.
There is no launcher and no menu in between: the app opens straight into the game.

Every app runs on the Hydroid engine. It loads the game's own arm64 code, translates it to
x86-64 as it runs, and answers the game's calls to Android itself (graphics, sound, input,
files, network). There is no Android image underneath.

Because every game has its own app, each one keeps the engine build and the settings it was
tested with. A change made for one game can't break another.

## Games

| Game | Version tested | State |
|---|---|---|
| Subway Surfers | 3.70.3 | In testing |

## Installing

1. Install the game's app folder as a homebrew title (for example with ShadowMountPlus).
2. Put your own copy of the game (`.apk`, `.xapk` or `.apks`) in the app's `games` folder over FTP:
   `/data/homebrew/<title id>/games`
3. Start the app from the home screen.

The apps don't include the games. Use your own copy.

## In a game

| Button | What it does |
|---|---|
| Options (hold) | Quick menu: resume, stats overlay, keymap editor, close game |

## License

GPL-3.0. See [LICENSE](LICENSE).

The Android robot is reproduced or modified from work created and shared by Google and used
according to terms described in the Creative Commons 3.0 Attribution License.
