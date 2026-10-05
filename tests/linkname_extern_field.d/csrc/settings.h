#ifndef SETTINGS_H
#define SETTINGS_H
/* A C API whose field names are kama keywords: `base` and `match`. */
typedef struct BaseSettings { int up; int slope; } BaseSettings;
typedef struct Settings { BaseSettings base; int match; float mass; } Settings;
void settings_init(Settings* s);
int settings_sum(const Settings* s);
#endif
