// app_runner_device's C half (see seen.h): in an Android app, the window filled once and every event counted and
// printed as it arrives; everywhere else, nothing to draw and nothing counted.
#include "seen.h"
#if defined(KAMA_ANDROID_APP)
#include <android/input.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <stdbool.h>
#include <stdio.h>
extern struct android_app* kama__android_app;
extern bool (*kama__android_input)(void*);
static ANativeWindow* drawn;
static int32_t counts[10];
int32_t seen_draw(void) {
    ANativeWindow* w = kama__android_app ? kama__android_app->window : NULL;
    if (!w) return 0;
    if (w == drawn) return 1;
    ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(w, &b, NULL) != 0) return 0;
    for (int y = 0; y < b.height; y++) {
        uint32_t* row = (uint32_t*)b.bits + (size_t)y * b.stride;
        for (int x = 0; x < b.width; x++) row[x] = 0xff336699u;
    }
    ANativeWindow_unlockAndPost(w);
    drawn = w;
    return 1;
}
static bool seen(void* ev) {
    AInputEvent* e = (AInputEvent*)ev;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
        const int a = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, tool = AMotionEvent_getToolType(e, 0);
        const int base = tool == AMOTION_EVENT_TOOL_TYPE_MOUSE ? 0 : tool == AMOTION_EVENT_TOOL_TYPE_STYLUS ? 5 : 2;
        const char* who = base == 0 ? "mouse" : base == 5 ? "pen" : "touch";
        const char* act = a == AMOTION_EVENT_ACTION_DOWN ? "down" : a == AMOTION_EVENT_ACTION_UP ? "up"
                        : a == AMOTION_EVENT_ACTION_MOVE ? "move" : "other";
        if (a == AMOTION_EVENT_ACTION_DOWN) counts[base]++;
        else if (a == AMOTION_EVENT_ACTION_UP) counts[base == 2 ? 4 : base + 1]++;
        else if (a == AMOTION_EVENT_ACTION_MOVE && base == 2) counts[3]++;
        printf("   %s %s %.0f,%.0f\n", who, act, AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0));
    } else {
        const int down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        counts[down ? 7 : 8]++;
        if (down && (AKeyEvent_getMetaState(e) & AMETA_SHIFT_ON)) counts[9]++;
        printf("   key %s %d\n", down ? "down" : "up", AKeyEvent_getKeyCode(e));
    }
    fflush(stdout);
    return true;
}
void seen_listen(void) { kama__android_input = seen; }
int32_t seen_count(int32_t kind) { return kind >= 0 && kind < 10 ? counts[kind] : 0; }
int32_t seen_app(void) { return 1; }
#else
int32_t seen_draw(void) { return 1; }
void seen_listen(void) { }
int32_t seen_count(int32_t kind) { (void)kind; return 0; }
int32_t seen_app(void) { return 0; }
#endif
