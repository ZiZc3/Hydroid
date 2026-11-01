#ifndef UI_SOUND_H
#define UI_SOUND_H

// The menus' sounds (the Quick menu), built in (title/make_ui_sounds.py; one sound for every id).
enum { SND_CLICK, SND_LAUNCH, SND_OPEN, SND_CLOSE, SND_CHANGE, SND_CONFIRM, SND_FAV, SND_STEP };

int ui_sound_open(void);  // its own sound port and mixer; 0 = ok
void ui_sound_close(void); // the port let go
void ui_sound_play(int id, float gain); // gain 1 = as made


#endif
