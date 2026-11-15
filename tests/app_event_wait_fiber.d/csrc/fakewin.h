#ifndef FAKEWIN_H
#define FAKEWIN_H
#include <stdint.h>
#include <stdbool.h>
// A window package's event queue, as GLFW keeps one: a wait that sleeps until an event comes, a post comes or a timeout
// passes (glfwWaitEventsTimeout / glfwWaitEvents), a post callable from any thread (glfwPostEmptyEvent), and input,
// which the OS delivers from wherever it likes. The wait answers whether it delivered input.
bool fakewin_wait(int64_t ns, bool forever);
void fakewin_post(void);
void fakewin_input(void);
int32_t fakewin_posts(void);
int32_t fakewin_inputs(void);
#endif
