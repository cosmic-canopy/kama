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
// The test's own clock: until the loop waits in a wait it entered after `after` of them, answering how many it has
// entered; the timer fiber's mark; and until that mark, answering how many waits the loop had entered by then.
int32_t fakewin_await(int32_t after);
void fakewin_timer(void);
int32_t fakewin_timer_waits(void);
#endif
