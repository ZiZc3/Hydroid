# Porting a game

Hydroid runs a game's own 64-bit ARM code on the PS5 (dynarmic) and answers the Android calls
it makes: the C library, OpenGL ES / Vulkan, audio, input, files, network, and the game's Java
side (`src/java_layer.c`). A port is the game's app plus whatever that game needs that Hydroid
doesn't answer yet.

## 1. Try it

1. Build the engine ([BUILD.md](BUILD.md)).
2. Make a folder for the game in `games/`, as the others are:
   - `game.json`: `name`, `package` (e.g. `com.imangi.templerun2`), `titleId` (`PPSA` + 5 digits,
     not used by another app on your console), `label` (16 letters/digits), `testedVersion`
   - `icon0.png` (the game's icon), `background.png` (any size, cut to 16:9)
3. `python3 tools/make_app.py games/<name> --engine dist/PPSA00000`, copy the app to the console,
   put the game's file in its `game` folder, start it.
4. Read its `android.log` (or `tools/live_log.py` while it runs).

Only 64-bit games run (`lib/arm64-v8a`). Supported engines: Unity (IL2CPP), Cocos2d-x,
Halfbrick's Mortar, Godot, Unreal (GameActivity), NativeActivity / GameActivity games.

## 2. Reading the log

| You see | Look at |
|---|---|
| `java: ... -> NOT FOUND`, `stub ... called` | a Java method the game calls that has no answer: add one in `src/java_layer.c` |
| `missing import: <name> (ARM stub returns 0)` | a C function the game imports that has no answer: `src/arm_imports.c`, `src/libc_extra.c` |
| a crash report (`arm: thread report`, a library + offset) | where the game stopped, and its last calls |
| `unity: N fps, slowest frame ...` | frame times every 5 s; `hitch` reports show what every thread was doing |

## 3. Fixes go per game

A fix for one game can break another, so game-specific fixes are switched on by package name.
Each list is a `#define ..._GAMES "com.a,com.b"` in the source (a name can be a prefix:
`com.imangi.`), checked with `droid_game_in()`:

| List | What it does |
|---|---|
| `FAST_JIT_GAMES`, `PARALLEL_WARM_GAMES` (`arm_cpu.cpp`) | faster ARM translation, translated ahead on more threads |
| `ARM_MEMMOVE_GAMES` (`arm_imports.c`) | memmove as ARM code (games that call it millions of times) |
| `PHONE_VIEW_GAMES` (`app_main.c`) | portrait games: a phone-shaped screen in the middle |
| `PAD_FIRST_GAMES`, `PAD_FIRST_CURSOR_GAMES` (`unity_host.c`) | the game's own controller support, from the start |
| `PAD_KEYBOARD_GAMES` / `PLAY_PC_GAMES` | games with keyboard controls (Google Play Games on PC builds): the pad as those keys |
| `SWIPE_GAMES` (`unity_host.c`) | runners: the pad as swipes |
| `CURSOR_ONLY_GAMES`, `CURSOR_RIGHT_GAMES` | the cursor (right stick, R2 taps) and the touchpad |
| `MOTION_GAMES`, `TILT_LEVEL_GAMES` (`sensors.c`) | the DualSense's motion as the phone's tilt |

**Portrait games:** a game made only for an upright phone (its manifest's `screenOrientation`
is portrait) goes in `PHONE_VIEW_GAMES`: it's told it has a 1080x1920 screen and is drawn in the
middle of the TV. Don't force such a game wide: it has no layout for it, so its menus and
buttons come out stretched, cut or in black boxes.

The Java answers in `src/java_layer.c` are tables of `{ answer, class, package }`: an answer
with a package is only for that game.

## 4. Controls

Look for the game's own controller support first: it covers the menus too.

- **Unity:** search `assets/bin/Data/Managed/Metadata/global-metadata.dat` for `Gamepad`,
  `InControl`, `Rewired`, `Controller`. Found: add the package to `PAD_FIRST_GAMES`.
- **Keyboard controls for PC:** games checking `com.google.android.play.feature.HPE_EXPERIENCE`
  turn them on for Google Play Games on PC: `PLAY_PC_GAMES` and `PAD_KEYBOARD_GAMES`.
- **Touch only:** make a button layout with the Keymap Editor:
  1. Put an empty file named `keyedit.txt` in the app's folder on the console.
  2. In the game, hold **Options**: the Quick menu opens. Pick **Keymap Editor**.
  3. L1 / R1 pick a DualSense button, the stick aims, ✕ places it on the game's button; the
     joystick sizes with L2 / R2; □ removes; △ saves.
  4. Copy the saved `data/games/<game>/touchmap.txt` from the console into `games/<name>/`,
     delete `keyedit.txt`. `make_app.py` puts the layout in the app.

Positions are in the game's 1920x1080 points, so a layout fits every TV.

## 5. Debug tools

Write tool names, one per line, in `tools.txt` in the game's folder on the console
(`/data/homebrew/<title id>/data/games/<game>/tools.txt`) and start the game:

| Tool | Shows |
|---|---|
| `NetSpy` | every network call (lookups, connects, sends, receives) |
| `ThreadScope` | what each thread does, every few seconds |
| `ArmProf` | where the ARM code spends its time |
| `CallDiff` | the host calls each frame, to compare two runs |
| `JavaMap` | every Java class and method the game asks for |
| `TexSpy` | texture formats and uploads |
| `ShotBox` | screenshots into the game's folder |
| `FlipMeter`, `FlipRate` | frame pacing |

`xtool.txt` in the game's folder traces functions of the game's own code by library + offset
(arguments and results in the log); see `src/arm_trace.c`.

## 6. Adding an engine

Each engine Hydroid knows has a small "host" in `src/`: it plays the part of the engine's Java
side (Android's Activity, its GL thread and its input) and calls the engine's native functions in
the order the engine's own Java would.

| Engine | Host |
|---|---|
| Unity | `unity_host.c` |
| Cocos2d-x | `cocos_host.c` |
| Halfbrick's Mortar | `mortar_host.c` |
| Godot | `godot_host.c` |
| Unreal / GameActivity | `gameactivity_host.c` |
| NativeActivity | `native_activity.c` |
| Rockstar (RenderWare, its oswrapper Android ports) | `gtasa_host.c` |

A new one is two functions, as the others have:

```c
// a library of the game is this engine: one of its own natives is there
int myengine_detect(so_module *mod) { return so_sym(mod, "Java_com_example_engine_Native_init") != NULL; }

// the engine's Java side: its init calls, then its frame call in a loop; poll() hands over
// the DualSense's input each frame. Never returns.
void myengine_run(so_module *mod, const char *apk_path, myengine_poll_fn poll);
```

1. Read the engine's Java (decompile the game's APK, e.g. with jadx): which `native` methods its
   Activity / view / renderer call, in what order, with what arguments.
2. Write `src/myengine_host.c` and `.h`: `godot_host.c` (the smallest) and `mortar_host.c` are good
   ones to copy from. Java calls the engine makes back are answered in `src/java_layer.c`.
3. Add its detect / run to `run_apk` in `src/app_main.c`, next to the others.
4. Add `myengine_host` to the file list in `title/build-title.sh`.

The log says which host started (`Android: <engine> game (...)`), or that none matched the game.
