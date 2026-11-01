#ifndef FIREBASE_RC_H
#define FIREBASE_RC_H

// Firebase Remote Config from Google's servers for the games on firebase_rc.c's list (the game's
// own ids from its APK); every other game keeps java_layer.c's offline answers.
void frc_start(const char *version); // at the game's start (java_package set): last launch's + a fresh fetch
int frc_enabled(void);
int frc_wait(int ms);                // the game's fetch: 1 when there are values
int frc_activate(void);              // the game's activate: 1 when the values changed
const char *frc_value(const char *key); // Google's value for key, or NULL
int frc_count(void);
const char *frc_key(int i);

#endif
