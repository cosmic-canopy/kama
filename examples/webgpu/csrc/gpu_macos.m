// macOS compiles the native layer as Objective-C: the surface is a CAMetalLayer attached to the NSWindow's
// content view, and that takes `[ns_window.contentView setLayer:…]`. One source for every native OS, so the
// macOS build includes gpu.c rather than keeping a second copy; kama.json lists this file for OS_MACOS and
// gpu.c for every other native OS.
#include "gpu.c"
