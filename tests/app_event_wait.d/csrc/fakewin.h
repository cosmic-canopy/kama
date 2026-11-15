#ifndef FAKEWIN_H
#define FAKEWIN_H
#include <stdint.h>
#include <stdbool.h>
// A window package's event queue, as GLFW keeps one: a wait that sleeps until an event is posted or a timeout passes
// (glfwWaitEventsTimeout / glfwWaitEvents), and a post callable from any thread (glfwPostEmptyEvent).
void fakewin_wait(int64_t ns, bool forever);
void fakewin_post(void);
int32_t fakewin_waits_with_deadline(void);
int32_t fakewin_waits_forever(void);
int32_t fakewin_posts(void);
#endif
