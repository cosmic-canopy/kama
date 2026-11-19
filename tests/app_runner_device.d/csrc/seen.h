#include <stdint.h>
/* app_runner_device's C half. In an Android app the window is filled once, and each input event is counted by kind:
   0 mouse down, 1 mouse up, 2 touch down, 3 touch move, 4 touch up, 5 pen down, 6 pen up, 7 key down, 8 key up,
   9 a key down with Shift held. Everywhere else there is nothing to draw or count. */
int32_t seen_draw(void);              /* 1 once the window holds a frame: Android routes no touch to one before */
void seen_listen(void);               /* count every event from now on */
int32_t seen_count(int32_t kind);
int32_t seen_app(void);               /* 1 in an Android app */
