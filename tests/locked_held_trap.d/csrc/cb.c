#include "cb.h"
extern void lockedheld_on_event(void);
void cb_call_back(void) { lockedheld_on_event(); }
