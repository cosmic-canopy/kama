#include "settings.h"
void settings_init(Settings* s) { s->base.up = 1; s->base.slope = 45; s->match = 3; s->mass = 2.0f; }
int settings_sum(const Settings* s) { return s->base.up + s->base.slope + s->match + (int)s->mass; }
