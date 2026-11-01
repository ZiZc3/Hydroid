// ui_icons.h - the menus' icons: Hugeicons (Stroke Rounded, MIT), made by title/make_ui_icons.py
#ifndef UI_ICONS_H
#define UI_ICONS_H
#include <stdint.h>

enum {
    UI_ICON_SETTINGS,
    UI_ICON_REFRESH,
    UI_ICON_INFO,
    UI_ICON_PENCIL,
    UI_ICON_SCREEN,
    UI_ICON_GAMEPAD,
    UI_ICON_WIFI,
    UI_ICON_DRIVE,
    UI_ICON_SPEED,
    UI_ICON_PALETTE,
    UI_ICON_GRID,
    UI_ICON_PLAY,
    UI_ICON_CHART,
    UI_ICON_HOME,
    UI_ICON_STORE,
    UI_ICON_SEARCH,
    UI_ICON_DOWNLOAD,
    UI_ICON_ACCOUNT,
    UI_ICON_PAUSE,
    UI_ICON_CANCEL,
    UI_ICON_STAR,
    UI_ICON_COUNT
};
#define UI_ICON_COLS 4
#define UI_ICONS_S_CELL 48 // the small ones: 192x288
#define UI_ICONS_S_W 192
#define UI_ICONS_S_H 288
#define UI_ICONS_B_CELL 128 // the Quick menu's: 512x768
#define UI_ICONS_B_W 512
#define UI_ICONS_B_H 768
extern const uint8_t ui_icons_small[]; // coverage masks, rows top first
extern const uint8_t ui_icons_big[];

#endif
